/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QtGlobal>
#include <memory>

#ifdef Q_OS_WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>

#include <iostream>
#endif  // Q_OS_WIN32

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif  // Q_OS_UNIX

#include <glib-object.h>
#include <glib.h>
#include <gst/gst.h>

#include <QDir>
#include <QFont>
#include <QLibraryInfo>
#include <QNetworkProxyFactory>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSslSocket>
#include <QSysInfo>
#include <QTextCodec>
#include <QTranslator>
#include <QtConcurrentRun>
#include <QtDebug>

#include "config.h"
#include "core/application.h"
#include "core/commandlineoptions.h"
#include "core/crashreporting.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/mac_startup.h"
#include "core/metatypes.h"
#include "core/network.h"
#include "core/networkproxyfactory.h"
#include "core/potranslator.h"
#include "core/song.h"
#include "core/ubuntuunityhack.h"
#include "core/utilities.h"
#include "engines/enginebase.h"
#include "qtsingleapplication.h"
//#include "qtsinglecoreapplication.h"
#include "smartplaylists/generator.h"
#include "singleapplication/RunGuard.h"
#include "tagreadermessages.pb.h"
#include "ui/iconloader.h"
#include "ui/mainwindow.h"
#include "ui/systemtrayicon.h"
#include "version.h"
#include "widgets/osd.h"

#ifdef Q_OS_DARWIN
#include <sys/resource.h>
#include <sys/sysctl.h>
#endif

#ifdef HAVE_LIBLASTFM
#include "internet/lastfm/lastfmservice.h"
#else
class LastFMService;
#endif

#ifdef HAVE_DBUS
#include <QDBusArgument>
#include <QImage>

#include "core/mpris.h"
#include "core/mpris2.h"

QDBusArgument& operator<<(QDBusArgument& arg, const QImage& image);
const QDBusArgument& operator>>(const QDBusArgument& arg, QImage& image);
#endif

// Load sqlite plugin on windows and mac.
#include <QtPlugin>
Q_IMPORT_PLUGIN(QSQLiteDriverPlugin)

namespace {

void LoadTranslation(const QString& prefix, const QString& path,
                     const QString& language) {
  QTranslator* t = new PoTranslator;
  if (t->load(prefix + "_" + language, path))
    QCoreApplication::installTranslator(t);
  else
    delete t;
}

void IncreaseFDLimit() {
#ifdef Q_OS_DARWIN
  // Bump the soft limit for the number of file descriptors from the default of
  // 256 to
  // the maximum (usually 10240).
  struct rlimit limit;
  getrlimit(RLIMIT_NOFILE, &limit);

  // getrlimit() lies about the hard limit so we have to check sysctl.
  int max_fd = 0;
  size_t len = sizeof(max_fd);
  sysctlbyname("kern.maxfilesperproc", &max_fd, &len, nullptr, 0);

  limit.rlim_cur = max_fd;
  int ret = setrlimit(RLIMIT_NOFILE, &limit);

  if (ret == 0) {
    qLog(Debug) << "Max fd:" << max_fd;
  }
#endif
}

void SetEnv(const char* key, const QString& value) {
#ifdef Q_OS_WIN32
  putenv(QString("%1=%2").arg(key, value).toLocal8Bit().constData());
#else
  setenv(key, value.toLocal8Bit().constData(), 1);
#endif
}

// This must be done early so that the spotify blob process also picks up
// these environment variables.
void SetGstreamerEnvironment() {
  QString scanner_path;
  QString plugin_path;
  QString registry_filename;

// On windows and mac we bundle the gstreamer plugins with clementine
#ifdef USE_BUNDLE
#if defined(Q_OS_DARWIN)
  scanner_path = QCoreApplication::applicationDirPath() + "/" + USE_BUNDLE_DIR +
                 "/gst-plugin-scanner";
  plugin_path = QCoreApplication::applicationDirPath() + "/" + USE_BUNDLE_DIR +
                "/gstreamer";
#elif defined(Q_OS_WIN32)
  plugin_path = QCoreApplication::applicationDirPath() + "/gstreamer-plugins";
#endif
#endif

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN)
  registry_filename =
      Utilities::GetConfigPath(Utilities::Path_GstreamerRegistry);
#endif

  if (!scanner_path.isEmpty()) SetEnv("GST_PLUGIN_SCANNER", scanner_path);

  if (!plugin_path.isEmpty()) {
    SetEnv("GST_PLUGIN_PATH", plugin_path);
    // Never load plugins from anywhere else.
    SetEnv("GST_PLUGIN_SYSTEM_PATH", plugin_path);
  }

  if (!registry_filename.isEmpty()) {
    SetEnv("GST_REGISTRY", registry_filename);
  }

#if defined(Q_OS_DARWIN) && defined(USE_BUNDLE)
  SetEnv("GIO_EXTRA_MODULES", QCoreApplication::applicationDirPath() + "/" +
                                  USE_BUNDLE_DIR + "/gio-modules");
#endif

  SetEnv("PULSE_PROP_media.role", "music");
}

void ParseAProto() {
  const QByteArray data = QByteArray::fromHex(
      "08001a8b010a8801b2014566696c653a2f2f2f453a2f4d7573696b2f28414c42554d2"
      "9253230476f74616e25323050726f6a6563742532302d253230416d6269656e742532"
      "304c6f756e67652e6d786dba012a28414c42554d2920476f74616e2050726f6a65637"
      "4202d20416d6269656e74204c6f756e67652e6d786dc001c7a7efd104c801bad685e4"
      "04d001eeca32");
  cpb::tagreader::Message message;
  message.ParseFromArray(data.constData(), data.size());
}

void CheckPortable() {
  QDir appDir(QApplication::applicationDirPath());
  // First look for legacy data location.
  QDir d(appDir.filePath(Application::kLegacyPortableDataDir));
  // Key off of database file since config name may vary depending on platform.
  if (d.exists("clementine.db")) {
    // We are portable. Set the bool and change the qsettings path
    qLog(Info) << "Using legacy portable data location:" << d.path();
    Application::kIsPortable = true;
    Application::kPortableDataDir = Application::kLegacyPortableDataDir;

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, d.path());
    return;
  }

  d = appDir.filePath(Application::kDefaultPortableDataDir);
  if (d.exists()) {
    // We are portable. Set the bool and change the qsettings path
    qLog(Info) << "Using portable data location:" << d.path();
    Application::kIsPortable = true;
    Application::kPortableDataDir = Application::kDefaultPortableDataDir;

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, d.path());
    return;
  }
  qLog(Info) << "Using default config locations.";
}

}  // namespace

#ifdef HAVE_GIO
#undef signals  // Clashes with GIO, and not needed in this file
#include <gio/gio.h>

namespace {

void ScanGIOModulePath() {
  QString gio_module_path;

#if defined(Q_OS_WIN32)
  gio_module_path = QCoreApplication::applicationDirPath() + "/gio-modules";
#endif

  if (!gio_module_path.isEmpty()) {
    qLog(Debug) << "Adding GIO module path:" << gio_module_path;
    QByteArray bytes = gio_module_path.toLocal8Bit();
    g_io_modules_scan_all_in_directory(bytes.data());
  }
}

}  // namespace
#endif  // HAVE_GIO

int main(int argc, char* argv[]) {
  if (CrashReporting::SendCrashReport(argc, argv)) {
    return 0;
  }

  CrashReporting crash_reporting;

#ifdef Q_OS_DARWIN
  // Do Mac specific startup to get media keys working.
  // This must go before QApplication initialisation.
  mac::MacMain();
#endif

  QCoreApplication::setApplicationName("Clementine");
  QCoreApplication::setApplicationVersion(CLEMENTINE_VERSION_DISPLAY);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setOrganizationName("Clementine");
  QCoreApplication::setOrganizationDomain("clementine-player.org");

// This makes us show up nicely in gnome-volume-control
#if !GLIB_CHECK_VERSION(2, 36, 0)
  g_type_init();  // Deprecated in glib 2.36.0
#endif
  g_set_application_name(QCoreApplication::applicationName().toLocal8Bit());

  RegisterMetaTypes();

  // Initialise logging.  Log levels are set after the commandline options are
  // parsed below.
  logging::Init();
  g_log_set_default_handler(reinterpret_cast<GLogFunc>(&logging::GLog),
                            nullptr);

  CommandlineOptions options(argc, argv);

  {
    // Only start a core application now so we can check if there's another
    // Clementine running without needing an X server.
    // This MUST be done before parsing the commandline options so QTextCodec
    // gets the right system locale for filenames.
    
    RunGuard guard("Ogiewoogiewoogie");
    if(!guard.tryToRun())
      return 0;

    // Parse commandline options - need to do this before starting the
    // full QApplication so it works without an X server
    if (!options.Parse()) return 1;
    logging::SetLevels(options.log_levels());


  // Output the version, so when people attach log output to bug reports they
  // don't have to tell us which version they're using.
  qLog(Info) << "Clementine-qt5" << CLEMENTINE_VERSION_DISPLAY;

  // Seed the random number generators.
  time_t t = time(nullptr);
  // rand is still used in 3rdParty
  srand(t);
#if (QT_VERSION < QT_VERSION_CHECK(5, 10, 0))
  // Deprecated in 1.15. Starting in 5.10, QRandomGenerator::global provides a
  // securely seeded PRNG.
  qsrand(t);
#endif

  IncreaseFDLimit();

  QtSingleApplication a(argc, argv);

#ifdef HAVE_LIBLASTFM
  lastfm::ws::ApiKey = LastFMService::kApiKey;
  lastfm::ws::SharedSecret = LastFMService::kSecret;
  lastfm::setNetworkAccessManager(new NetworkAccessManager);
#endif

  // A bug in Qt means the wheel_scroll_lines setting gets ignored and replaced
  // with the default value of 3 in QApplicationPrivate::initialize.
  {
    QSettings qt_settings(QSettings::UserScope, "Trolltech");
    qt_settings.beginGroup("Qt");
    QApplication::setWheelScrollLines(
        qt_settings.value("wheelScrollLines", QApplication::wheelScrollLines())
            .toInt());
  }

#if defined(Q_OS_DARWIN) && defined(USE_BUNDLE)
  qLog(Debug) << "Looking for resources in" +
                     QCoreApplication::applicationDirPath() + "/" +
                     USE_BUNDLE_DIR;
  QCoreApplication::setLibraryPaths(QStringList()
                                    << QCoreApplication::applicationDirPath() +
                                           "/" + USE_BUNDLE_DIR);
#endif

  a.setQuitOnLastWindowClosed(false);
  // Do this check again because another instance might have started by now
  if (a.isRunning() &&
      a.sendMessage(QString::fromLatin1(options.Serialize()), 5000)) {
    return 0;
  }

#ifndef Q_OS_DARWIN
  // Gnome on Ubuntu has menu icons disabled by default.  I think that's a bad
  // idea, and makes some menus in Clementine look confusing.
  QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, false);
#else
  QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, true);
#endif

  SetGstreamerEnvironment();

// Set the permissions on the config file on Unix - it can contain passwords
// for internet services so it's important that other users can't read it.
// On Windows these are stored in the registry instead.
#ifdef Q_OS_UNIX
  {
    QSettings s;

    // Create the file if it doesn't exist already
    if (!QFile::exists(s.fileName())) {
      QFile file(s.fileName());
      file.open(QIODevice::WriteOnly);
    }

    // Set -rw-------
    QFile::setPermissions(s.fileName(), QFile::ReadOwner | QFile::WriteOwner);
  }
#endif

// Set the name of the app desktop file as per the freedesktop specifications
// This is needed on Wayland for the main window to show the correct icon
#if QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
  QGuiApplication::setDesktopFileName("org.clementine_player.Clementine");
#endif

  // Resources
  Q_INIT_RESOURCE(data);
#ifdef HAVE_TRANSLATIONS
  Q_INIT_RESOURCE(translations);
#endif

  // Has the user forced a different language?
  QString override_language = options.language();
  if (override_language.isEmpty()) {
    QSettings s;
    s.beginGroup("General");
    override_language = s.value("language").toString();
  }

  const QString language = override_language.isEmpty()
                               ? Utilities::SystemLanguageName()
                               : override_language;

  // Translations
  LoadTranslation("qt", QLibraryInfo::location(QLibraryInfo::TranslationsPath),
                  language);
  LoadTranslation("clementine", ":/translations", language);
  LoadTranslation("clementine", a.applicationDirPath(), language);
  LoadTranslation("clementine", QDir::currentPath(), language);

  // Icons
  IconLoader::Init();

  // This is a nasty hack to ensure that everything in libprotobuf is
  // initialised in the main thread.  It fixes issue 3265 but nobody knows why.
  // Don't remove this unless you can reproduce the error that it fixes.
  ParseAProto();
  QtConcurrent::run(&ParseAProto);

  Application app;
  QObject::connect(&a, SIGNAL(aboutToQuit()), &app, SLOT(SaveSettings_()));
  app.set_language_name(language);

  // Network proxy
  QNetworkProxyFactory::setApplicationProxyFactory(
      NetworkProxyFactory::Instance());

#ifdef Q_OS_LINUX
  // In 11.04 Ubuntu decided that the system tray should be reserved for certain
  // whitelisted applications.  Clementine will override this setting and insert
  // itself into the list of whitelisted apps.
  UbuntuUnityHack hack;
#endif  // Q_OS_LINUX

  // Create the tray icon and OSD
  std::unique_ptr<SystemTrayIcon> tray_icon(
      SystemTrayIcon::CreateSystemTrayIcon());
  OSD osd(tray_icon.get(), &app);

#ifdef HAVE_DBUS
  mpris::Mpris mpris(&app);
#endif

  // Window
  MainWindow w(&app, tray_icon.get(), &osd, options);
#ifdef Q_OS_DARWIN
  mac::EnableFullScreen(w);
#endif  // Q_OS_DARWIN
#ifdef HAVE_GIO
  ScanGIOModulePath();
#endif
#ifdef HAVE_DBUS
  QObject::connect(&mpris, SIGNAL(RaiseMainWindow()), &w, SLOT(Raise()));
#endif
  QObject::connect(&a, SIGNAL(messageReceived(QString)), &w,
                   SLOT(CommandlineOptionsReceived(QString)));

  // Use a queued connection so the invokation occurs after the application
  // loop starts.
  QMetaObject::invokeMethod(&app, "Starting", Qt::QueuedConnection);

  int ret = a.exec();

  return ret;
}
