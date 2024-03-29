/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "qmljsplugindumper.h"
#include "qmljsmodelmanager.h"

#include <qmljs/qmljsinterpreter.h>
#include <projectexplorer/session.h>
#include <coreplugin/messagemanager.h>
#include <utils/filesystemwatcher.h>
#include <utils/fileutils.h>

#include <QDir>

using namespace LanguageUtils;
using namespace QmlJS;
using namespace QmlJSTools;
using namespace QmlJSTools::Internal;

PluginDumper::PluginDumper(ModelManager *modelManager)
    : QObject(modelManager)
    , m_modelManager(modelManager)
    , m_pluginWatcher(0)
{
    qRegisterMetaType<QmlJS::ModelManagerInterface::ProjectInfo>("QmlJS::ModelManagerInterface::ProjectInfo");
}

Utils::FileSystemWatcher *PluginDumper::pluginWatcher()
{
    if (!m_pluginWatcher) {
        m_pluginWatcher = new Utils::FileSystemWatcher(this);
        m_pluginWatcher->setObjectName(QLatin1String("PluginDumperWatcher"));
        connect(m_pluginWatcher, SIGNAL(fileChanged(QString)),
                this, SLOT(pluginChanged(QString)));
    }
    return m_pluginWatcher;
}

void PluginDumper::loadBuiltinTypes(const QmlJS::ModelManagerInterface::ProjectInfo &info)
{
    // move to the owning thread
    metaObject()->invokeMethod(this, "onLoadBuiltinTypes",
                               Q_ARG(QmlJS::ModelManagerInterface::ProjectInfo, info));
}

void PluginDumper::loadPluginTypes(const QString &libraryPath, const QString &importPath, const QString &importUri, const QString &importVersion)
{
    // move to the owning thread
    metaObject()->invokeMethod(this, "onLoadPluginTypes",
                               Q_ARG(QString, libraryPath),
                               Q_ARG(QString, importPath),
                               Q_ARG(QString, importUri),
                               Q_ARG(QString, importVersion));
}

void PluginDumper::scheduleRedumpPlugins()
{
    // move to the owning thread
    metaObject()->invokeMethod(this, "dumpAllPlugins", Qt::QueuedConnection);
}

void PluginDumper::scheduleMaybeRedumpBuiltins(const QmlJS::ModelManagerInterface::ProjectInfo &info)
{
    // move to the owning thread
    metaObject()->invokeMethod(this, "dumpBuiltins", Qt::QueuedConnection,
                               Q_ARG(QmlJS::ModelManagerInterface::ProjectInfo, info));
}

void PluginDumper::onLoadBuiltinTypes(const QmlJS::ModelManagerInterface::ProjectInfo &info, bool force)
{
    if (info.qmlDumpPath.isEmpty() || info.qtImportsPath.isEmpty())
        return;

    const QString importsPath = QDir::cleanPath(info.qtImportsPath);
    if (m_runningQmldumps.values().contains(importsPath))
        return;

    LibraryInfo builtinInfo;
    if (!force) {
        const Snapshot snapshot = m_modelManager->snapshot();
        builtinInfo = snapshot.libraryInfo(info.qtImportsPath);
        if (builtinInfo.isValid())
            return;
    }
    builtinInfo = LibraryInfo(LibraryInfo::Found);
    m_modelManager->updateLibraryInfo(info.qtImportsPath, builtinInfo);

    // prefer QTDIR/imports/builtins.qmltypes if available
    const QString builtinQmltypesPath = info.qtImportsPath + QLatin1String("/builtins.qmltypes");
    if (QFile::exists(builtinQmltypesPath)) {
        loadQmltypesFile(QStringList(builtinQmltypesPath), info.qtImportsPath, builtinInfo);
        return;
    }
    // QTDIR/imports/QtQuick1/builtins.qmltypes was used in developer builds of 5.0.0, 5.0.1
    const QString builtinQmltypesPath2 = info.qtImportsPath
            + QLatin1String("/QtQuick1/builtins.qmltypes");
    if (QFile::exists(builtinQmltypesPath2)) {
        loadQmltypesFile(QStringList(builtinQmltypesPath2), info.qtImportsPath, builtinInfo);
        return;
    }

    // run qmldump
    QProcess *process = new QProcess(this);
    process->setEnvironment(info.qmlDumpEnvironment.toStringList());
    connect(process, SIGNAL(finished(int)), SLOT(qmlPluginTypeDumpDone(int)));
    connect(process, SIGNAL(error(QProcess::ProcessError)), SLOT(qmlPluginTypeDumpError(QProcess::ProcessError)));
    QStringList args(QLatin1String("--builtins"));
    process->start(info.qmlDumpPath, args);
    m_runningQmldumps.insert(process, info.qtImportsPath);
    m_qtToInfo.insert(info.qtImportsPath, info);
}

static QString makeAbsolute(const QString &path, const QString &base)
{
    if (QFileInfo(path).isAbsolute())
        return path;
    return QString::fromLatin1("%1%2%3").arg(base, QDir::separator(), path);
}

void PluginDumper::onLoadPluginTypes(const QString &libraryPath, const QString &importPath, const QString &importUri, const QString &importVersion)
{
    const QString canonicalLibraryPath = QDir::cleanPath(libraryPath);
    if (m_runningQmldumps.values().contains(canonicalLibraryPath))
        return;
    const Snapshot snapshot = m_modelManager->snapshot();
    const LibraryInfo libraryInfo = snapshot.libraryInfo(canonicalLibraryPath);
    if (libraryInfo.pluginTypeInfoStatus() != LibraryInfo::NoTypeInfo)
        return;

    // avoid inserting the same plugin twice
    int index;
    for (index = 0; index < m_plugins.size(); ++index) {
        if (m_plugins.at(index).qmldirPath == libraryPath)
            break;
    }
    if (index == m_plugins.size())
        m_plugins.append(Plugin());

    Plugin &plugin = m_plugins[index];
    plugin.qmldirPath = canonicalLibraryPath;
    plugin.importPath = importPath;
    plugin.importUri = importUri;
    plugin.importVersion = importVersion;

    // add default qmltypes file if it exists
    const QLatin1String defaultQmltypesFileName("plugins.qmltypes");
    const QString defaultQmltypesPath = makeAbsolute(defaultQmltypesFileName, canonicalLibraryPath);
    if (QFile::exists(defaultQmltypesPath))
        plugin.typeInfoPaths += defaultQmltypesPath;

    // add typeinfo files listed in qmldir
    foreach (const QmlDirParser::TypeInfo &typeInfo, libraryInfo.typeInfos())
        plugin.typeInfoPaths += makeAbsolute(typeInfo.fileName, canonicalLibraryPath);

    // watch plugin libraries
    foreach (const QmlDirParser::Plugin &plugin, snapshot.libraryInfo(canonicalLibraryPath).plugins()) {
        const QString pluginLibrary = resolvePlugin(canonicalLibraryPath, plugin.path, plugin.name);
        if (!pluginLibrary.isEmpty()) {
            if (!pluginWatcher()->watchesFile(pluginLibrary))
                pluginWatcher()->addFile(pluginLibrary, Utils::FileSystemWatcher::WatchModifiedDate);
            m_libraryToPluginIndex.insert(pluginLibrary, index);
        }
    }

    // watch library qmltypes file
    if (!plugin.typeInfoPaths.isEmpty()) {
        foreach (const QString &path, plugin.typeInfoPaths) {
            if (!QFile::exists(path))
                continue;
            if (!pluginWatcher()->watchesFile(path))
                pluginWatcher()->addFile(path, Utils::FileSystemWatcher::WatchModifiedDate);
            m_libraryToPluginIndex.insert(path, index);
        }
    }

    dump(plugin);
}

void PluginDumper::dumpBuiltins(const QmlJS::ModelManagerInterface::ProjectInfo &info)
{
    // if the builtin types were generated with a different qmldump, regenerate!
    if (m_qtToInfo.contains(info.qtImportsPath)) {
        QmlJS::ModelManagerInterface::ProjectInfo oldInfo = m_qtToInfo.value(info.qtImportsPath);
        if (oldInfo.qmlDumpPath != info.qmlDumpPath
                || oldInfo.qmlDumpEnvironment != info.qmlDumpEnvironment) {
            m_qtToInfo.remove(info.qtImportsPath);
            onLoadBuiltinTypes(info, true);
        }
    }
}

void PluginDumper::dumpAllPlugins()
{
    foreach (const Plugin &plugin, m_plugins) {
        dump(plugin);
    }
}

static QString noTypeinfoError(const QString &libraryPath)
{
    return PluginDumper::tr("QML module does not contain information about components contained in plugins\n\n"
                            "Module path: %1\n"
                            "See \"Using QML Modules with Plugins\" in the documentation.").arg(
                libraryPath);
}

static QString qmldumpErrorMessage(const QString &libraryPath, const QString &error)
{
    return noTypeinfoError(libraryPath) + QLatin1String("\n\n") +
            PluginDumper::tr("Automatic type dump of QML module failed.\nErrors:\n%1").
            arg(error) + QLatin1Char('\n');
}

static QString qmldumpFailedMessage(const QString &libraryPath, const QString &error)
{
    QString firstLines =
            QStringList(error.split(QLatin1Char('\n')).mid(0, 10)).join(QLatin1String("\n"));
    return noTypeinfoError(libraryPath) + QLatin1String("\n\n") +
            PluginDumper::tr("Automatic type dump of QML module failed.\n"
                             "First 10 lines or errors:\n"
                             "\n"
                             "%1"
                             "\n"
                             "Check 'General Messages' output pane for details."
                             ).arg(firstLines);
}

static void printParseWarnings(const QString &libraryPath, const QString &warning)
{
    Core::MessageManager::write(
                PluginDumper::tr("Warnings while parsing qmltypes information of %1:\n"
                                 "%2").arg(libraryPath, warning),
                Core::MessageManager::Flash);
}

static QString qmlPluginDumpErrorMessage(QProcess *process)
{
    QString errorMessage;
#if QT_VERSION >= 0x050000
    const QString binary = QDir::toNativeSeparators(process->program());
#else
    const QString binary = QLatin1String("qmlplugindump");
#endif
    switch (process->error()) {
    case QProcess::FailedToStart:
        errorMessage = PluginDumper::tr("\"%1\" failed to start: %2").arg(binary, process->errorString());
        break;
    case QProcess::Crashed:
        errorMessage = PluginDumper::tr("\"%1\" crashed.").arg(binary);
        break;
    case QProcess::Timedout:
        errorMessage = PluginDumper::tr("\"%1\" timed out.").arg(binary);
        break;
    case QProcess::ReadError:
    case QProcess::WriteError:
        errorMessage = PluginDumper::tr("I/O error running \"%1\".").arg(binary);
        break;
    case QProcess::UnknownError:
        if (process->exitCode())
            errorMessage = PluginDumper::tr("\"%1\" returned exit code %2.").arg(binary).arg(process->exitCode());
        break;
    }
#if QT_VERSION >= 0x050000
    errorMessage += QLatin1Char('\n') + PluginDumper::tr("Arguments: %1").arg(process->arguments().join(QLatin1Char(' ')));
#endif
    if (process->error() != QProcess::FailedToStart) {
        const QString stdErr = QString::fromLocal8Bit(process->readAllStandardError());
        if (!stdErr.isEmpty()) {
            errorMessage += QLatin1Char('\n');
            errorMessage += stdErr;
        }
    }
    return errorMessage;
}

void PluginDumper::qmlPluginTypeDumpDone(int exitCode)
{
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process)
        return;
    process->deleteLater();

    const QString libraryPath = m_runningQmldumps.take(process);
    if (libraryPath.isEmpty())
        return;
    const Snapshot snapshot = m_modelManager->snapshot();
    LibraryInfo libraryInfo = snapshot.libraryInfo(libraryPath);

    if (exitCode != 0) {
        const QString errorMessages = qmlPluginDumpErrorMessage(process);
        Core::MessageManager::write(qmldumpErrorMessage(libraryPath, errorMessages),
                                          Core::MessageManager::Flash);
        libraryInfo.setPluginTypeInfoStatus(LibraryInfo::DumpError, qmldumpFailedMessage(libraryPath, errorMessages));
    }

    const QByteArray output = process->readAllStandardOutput();
    QString error;
    QString warning;
    CppQmlTypesLoader::BuiltinObjects objectsList;
    QList<ModuleApiInfo> moduleApis;
    CppQmlTypesLoader::parseQmlTypeDescriptions(output, &objectsList, &moduleApis, &error, &warning,
                                                QLatin1String("<dump of ") + libraryPath + QLatin1String(">"));
    if (exitCode == 0) {
        if (!error.isEmpty()) {
            libraryInfo.setPluginTypeInfoStatus(LibraryInfo::DumpError,
                                                qmldumpErrorMessage(libraryPath, error));
        } else {
            libraryInfo.setMetaObjects(objectsList.values());
            libraryInfo.setModuleApis(moduleApis);
            libraryInfo.setPluginTypeInfoStatus(LibraryInfo::DumpDone);
        }

        if (!warning.isEmpty())
            printParseWarnings(libraryPath, warning);
    }

    m_modelManager->updateLibraryInfo(libraryPath, libraryInfo);
}

void PluginDumper::qmlPluginTypeDumpError(QProcess::ProcessError)
{
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process)
        return;
    process->deleteLater();

    const QString libraryPath = m_runningQmldumps.take(process);
    if (libraryPath.isEmpty())
        return;

    const QString errorMessages = qmlPluginDumpErrorMessage(process);
    Core::MessageManager::write(qmldumpErrorMessage(libraryPath, errorMessages),
                                      Core::MessageManager::Flash);
    if (!libraryPath.isEmpty()) {
        const Snapshot snapshot = m_modelManager->snapshot();
        LibraryInfo libraryInfo = snapshot.libraryInfo(libraryPath);
        libraryInfo.setPluginTypeInfoStatus(LibraryInfo::DumpError, qmldumpFailedMessage(libraryPath, errorMessages));
        m_modelManager->updateLibraryInfo(libraryPath, libraryInfo);
    }
}

void PluginDumper::pluginChanged(const QString &pluginLibrary)
{
    const int pluginIndex = m_libraryToPluginIndex.value(pluginLibrary, -1);
    if (pluginIndex == -1)
        return;

    const Plugin &plugin = m_plugins.at(pluginIndex);
    dump(plugin);
}

void PluginDumper::loadQmltypesFile(const QStringList &qmltypesFilePaths,
                                    const QString &libraryPath,
                                    QmlJS::LibraryInfo libraryInfo)
{
    QStringList errors;
    QStringList warnings;
    QList<FakeMetaObject::ConstPtr> objects;
    QList<ModuleApiInfo> moduleApis;

    foreach (const QString &qmltypesFilePath, qmltypesFilePaths) {
        Utils::FileReader reader;
        if (!reader.fetch(qmltypesFilePath, QFile::Text)) {
            errors += reader.errorString();
            continue;
        }

        QString error;
        QString warning;
        CppQmlTypesLoader::BuiltinObjects newObjects;
        QList<ModuleApiInfo> newModuleApis;
        CppQmlTypesLoader::parseQmlTypeDescriptions(reader.data(), &newObjects, &newModuleApis, &error, &warning, qmltypesFilePath);
        if (!error.isEmpty()) {
            errors += tr("Failed to parse '%1'.\nError: %2").arg(qmltypesFilePath, error);
        } else {
            objects += newObjects.values();
            moduleApis += newModuleApis;
        }
        if (!warning.isEmpty())
            warnings += warning;
    }

    libraryInfo.setMetaObjects(objects);
    libraryInfo.setModuleApis(moduleApis);
    if (errors.isEmpty()) {
        libraryInfo.setPluginTypeInfoStatus(LibraryInfo::TypeInfoFileDone);
    } else {
        errors.prepend(tr("Errors while reading typeinfo files:"));
        libraryInfo.setPluginTypeInfoStatus(LibraryInfo::TypeInfoFileError, errors.join(QLatin1String("\n")));
    }

    if (!warnings.isEmpty())
        printParseWarnings(libraryPath, warnings.join(QLatin1String("\n")));

    m_modelManager->updateLibraryInfo(libraryPath, libraryInfo);
}

void PluginDumper::dump(const Plugin &plugin)
{
    // if there are type infos, don't dump!
    if (!plugin.typeInfoPaths.isEmpty()) {
        const Snapshot snapshot = m_modelManager->snapshot();
        LibraryInfo libraryInfo = snapshot.libraryInfo(plugin.qmldirPath);
        if (!libraryInfo.isValid())
            return;

        loadQmltypesFile(plugin.typeInfoPaths, plugin.qmldirPath, libraryInfo);
        return;
    }

    ProjectExplorer::Project *activeProject = ProjectExplorer::SessionManager::startupProject();
    if (!activeProject)
        return;

    ModelManagerInterface::ProjectInfo info = m_modelManager->projectInfo(activeProject);

    if (!info.tryQmlDump || info.qmlDumpPath.isEmpty()) {
        const Snapshot snapshot = m_modelManager->snapshot();
        LibraryInfo libraryInfo = snapshot.libraryInfo(plugin.qmldirPath);
        if (!libraryInfo.isValid())
            return;

        QString errorMessage;
        if (!info.tryQmlDump) {
            errorMessage = noTypeinfoError(plugin.qmldirPath);
        } else {
            errorMessage = qmldumpErrorMessage(plugin.qmldirPath,
                    tr("Could not locate the helper application for dumping type information from C++ plugins.\n"
                       "Please build the qmldump application on the Qt version options page."));
        }

        libraryInfo.setPluginTypeInfoStatus(LibraryInfo::DumpError, errorMessage);
        m_modelManager->updateLibraryInfo(plugin.qmldirPath, libraryInfo);
        return;
    }

    QProcess *process = new QProcess(this);
    process->setEnvironment(info.qmlDumpEnvironment.toStringList());
    connect(process, SIGNAL(finished(int)), SLOT(qmlPluginTypeDumpDone(int)));
    connect(process, SIGNAL(error(QProcess::ProcessError)), SLOT(qmlPluginTypeDumpError(QProcess::ProcessError)));
    QStringList args;
    if (plugin.importUri.isEmpty()) {
        args << QLatin1String("--path");
        args << plugin.importPath;
        if (ComponentVersion(plugin.importVersion).isValid())
            args << plugin.importVersion;
    } else {
        if (info.qmlDumpHasRelocatableFlag)
            args << QLatin1String("-relocatable");
        args << plugin.importUri;
        args << plugin.importVersion;
        args << plugin.importPath;
    }
    process->start(info.qmlDumpPath, args);
    m_runningQmldumps.insert(process, plugin.qmldirPath);
}

/*!
  Returns the result of the merge of \a baseName with \a path, \a suffixes, and \a prefix.
  The \a prefix must contain the dot.

  \a qmldirPath is the location of the qmldir file.

  Adapted from QDeclarativeImportDatabase::resolvePlugin.
*/
QString PluginDumper::resolvePlugin(const QDir &qmldirPath, const QString &qmldirPluginPath,
                                    const QString &baseName, const QStringList &suffixes,
                                    const QString &prefix)
{
    QStringList searchPaths;
    searchPaths.append(QLatin1String("."));

    bool qmldirPluginPathIsRelative = QDir::isRelativePath(qmldirPluginPath);
    if (!qmldirPluginPathIsRelative)
        searchPaths.prepend(qmldirPluginPath);

    foreach (const QString &pluginPath, searchPaths) {

        QString resolvedPath;

        if (pluginPath == QLatin1String(".")) {
            if (qmldirPluginPathIsRelative)
                resolvedPath = qmldirPath.absoluteFilePath(qmldirPluginPath);
            else
                resolvedPath = qmldirPath.absolutePath();
        } else {
            resolvedPath = pluginPath;
        }

        QDir dir(resolvedPath);
        foreach (const QString &suffix, suffixes) {
            QString pluginFileName = prefix;

            pluginFileName += baseName;
            pluginFileName += suffix;

            QFileInfo fileInfo(dir, pluginFileName);

            if (fileInfo.exists())
                return fileInfo.absoluteFilePath();
        }
    }

    return QString();
}

/*!
  Returns the result of the merge of \a baseName with \a dir and the platform suffix.

  Adapted from QDeclarativeImportDatabase::resolvePlugin.

  \table
  \header \li Platform \li Valid suffixes
  \row \li Windows     \li \c .dll
  \row \li Unix/Linux  \li \c .so
  \row \li AIX  \li \c .a
  \row \li HP-UX       \li \c .sl, \c .so (HP-UXi)
  \row \li Mac OS X    \li \c .dylib, \c .bundle, \c .so
  \endtable

  Version number on unix are ignored.
*/
QString PluginDumper::resolvePlugin(const QDir &qmldirPath, const QString &qmldirPluginPath,
                                    const QString &baseName)
{
#if defined(Q_OS_WIN32) || defined(Q_OS_WINCE)
    return resolvePlugin(qmldirPath, qmldirPluginPath, baseName,
                         QStringList()
                         << QLatin1String("d.dll") // try a qmake-style debug build first
                         << QLatin1String(".dll"));
#elif defined(Q_OS_DARWIN)
    return resolvePlugin(qmldirPath, qmldirPluginPath, baseName,
                         QStringList()
                         << QLatin1String("_debug.dylib") // try a qmake-style debug build first
                         << QLatin1String(".dylib")
                         << QLatin1String(".so")
                         << QLatin1String(".bundle"),
                         QLatin1String("lib"));
#else  // Generic Unix
    QStringList validSuffixList;

#  if defined(Q_OS_HPUX)
/*
    See "HP-UX Linker and Libraries User's Guide", section "Link-time Differences between PA-RISC and IPF":
    "In PA-RISC (PA-32 and PA-64) shared libraries are suffixed with .sl. In IPF (32-bit and 64-bit),
    the shared libraries are suffixed with .so. For compatibility, the IPF linker also supports the .sl suffix."
 */
    validSuffixList << QLatin1String(".sl");
#   if defined __ia64
    validSuffixList << QLatin1String(".so");
#   endif
#  elif defined(Q_OS_AIX)
    validSuffixList << QLatin1String(".a") << QLatin1String(".so");
#  elif defined(Q_OS_UNIX)
    validSuffixList << QLatin1String(".so");
#  endif

    // Examples of valid library names:
    //  libfoo.so

    return resolvePlugin(qmldirPath, qmldirPluginPath, baseName, validSuffixList, QLatin1String("lib"));
#endif
}
