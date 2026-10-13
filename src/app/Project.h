#pragma once
#include "core/Model.h"
#include "media/Media.h"
#include <QMap>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <memory>

namespace sav {
struct Project {
    QString title = "Untitled show";
    int width = 1920, height = 1080;
    double fps = 24;
    Show show;
    QMap<QString, MediaAsset> assets;
    // Shared by undo snapshots so extracted sources remain available until their last user closes.
    std::shared_ptr<QTemporaryDir> packageDirectory;
};
QString newId();
Project newProject();
Project demoProject();
void validate(const Project& project);
void bindAudio(Project& project);
// Type-checks a manifest without touching media; asset paths resolve against directory.
Project parseProject(const QByteArray& json, const QString& directory);
// Loads each asset source, then binds and validates the complete project.
void loadProjectMedia(Project& project);
Project readProject(const QString& path);
// Shared manifest representation; an empty directory keeps absolute source paths.
QJsonObject projectJson(const Project& project, const QString& directory = {});
void writeProject(const Project& project, const QString& path);
bool isProjectPackage(const QString& path);
Project readProjectPackage(const QString& path);
void writeProjectPackage(const Project& project, const QString& path);
void materializeProjectMedia(Project& project, const QString& directory);
// Removes package extractions abandoned by crashed instances; call once at startup.
void removeStalePackageDirectories();
QString timecode(Frame position, double fps = 24);
Envelope* envelope(Clip& clip, int property);
// Sets a clip's timing; a left trim (start and offset moving together) shifts its automation.
void retime(Clip& clip, Frame start, Frame duration, Frame offset);
// Retimes a clip and applies the same start/duration/offset change to clips linked with it.
void retimeLinked(Project& project, const std::string& id, Frame start, Frame duration, Frame offset);
// The clip followed by its linked partners.
std::vector<std::string> linkedClips(const Project& project, const std::string& id);
const Envelope* envelope(const Clip& clip, int property);
double propertyValue(const Clip& clip, int property);
}
