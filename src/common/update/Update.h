#pragma once
// Rikugan's updates, the steps the Updater (Updater.h) runs on a worker thread: which GitHub release is the latest,
// whether it is newer than this Rikugan, and its package for this plugin downloaded, checked and put in place of the
// installed files. The same assets install.sh / install.ps1 install:
//   <releases>/download/v<version>/rikugan-<version>-<package>.<tar.gz|zip> and its .sha256
// where <package> is this build's host and platform ("binja-linux-x86_64", "ida-windows-x86_64" ...).
//
// A download is installed only when it matches the release's SHA-256, unpacks into regular files only, holds this
// plugin's file, and that file says it is the release's version (the marker every build carries, versionMarker()).
// Files are replaced the way a running host allows: renamed over (Linux, macOS: the running process keeps its copy),
// or, on Windows, the loaded file renamed aside (removeLeftovers() deletes it on the next start) and the new one put
// in its place. The host uses the new version once restarted.
//
// The network and the archives go through the system's curl and tar (Windows 10 and later ship both): IDA's Qt has no
// network module.
#include <functional>
#include <optional>

#include <QString>
#include <QStringList>
#include <QUrl>

namespace rk::update {

// "2.0.10" > "2.0.9"; a leading "v" is ignored. Up to four numbers; nullopt for anything else.
std::optional<QList<int>> parseVersion(const QString& text);
int compareVersions(const QString& a, const QString& b);  // <0, 0, >0 (an unparsable one is the older)

struct Release {
    QString tag;       // "v2.0.1"
    QString version;   // "2.0.1"
    QUrl page;         // .../releases/tag/v2.0.1
    QUrl package;      // .../releases/download/v2.0.1/rikugan-2.0.1-<package>.tar.gz (.zip on Windows)
    QUrl checksum;     // package + ".sha256"
};

// What this build is: its version, and the release package it was built as ("" when it was not: a developer's
// build, which never updates itself).
QString currentVersion();
QString packageName();
// The host the package is for: "Binary Ninja" or "IDA Pro" (from packageName()).
QString hostName(const QString& package);

// https://github.com/buzzer-re/Rikugan/releases, or RIKUGAN_UPDATE_URL (tests: a local server).
QUrl releasesUrl();
// The release whose page <releases>/latest landed on: <releases>/tag/v<version> on the same site, nothing else
// (no release published yet lands on <releases> itself).
std::optional<Release> releaseFromLanding(const QUrl& landed, const QUrl& releases, const QString& package);
// A check is due a day after the last one (or when the clock went back). 0: never checked.
bool isDue(qint64 lastCheckMs, qint64 nowMs);

// ---- blocking steps (a worker thread) ----

// Where <releases>/latest lands; empty with *error when it cannot be reached.
QUrl latestLanding(const QUrl& releases, QString* error);
// `url` saved at `file`; false with *error.
bool download(const QUrl& url, const QString& file, QString* error);
// The first word of a .sha256 file when it is a SHA-256 (lowercase); empty otherwise.
QString hashInChecksumFile(const QString& file);
QString sha256OfFile(const QString& file);
// The version a plugin file says it is (versionMarker()); empty when it carries none.
QString versionOfFile(const QString& file);
// The marker this build carries: "RIKUGAN-VERSION:<version>".
const char* versionMarker();

// The files of an unpacked package and where they go: `root` (the plugins folder, as the host has it) + each relative
// path. The package must hold the installed plugin's own file (`module`'s file name) at the same place below root.
struct Plan {
    QString root;
    QStringList files;  // relative to root and to the unpacked folder, '/' separators
};
std::optional<Plan> plan(const QString& unpacked, const QString& module, QString* error);

// Downloads the release's package for `module` into `work`, checks it and puts its files in place.
bool install(const Release& release, const QString& module, const QString& work, QString* error,
             const std::function<void(const QString&)>& progress = {});

// The files an update on Windows renamed aside (<file>.rikugan-old) below `root`; removed at the next start.
void removeLeftovers(const QString& root);

}  // namespace rk::update
