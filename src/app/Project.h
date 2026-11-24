#pragma once
#include "core/Model.h"
#include "media/Media.h"
#include <QMap>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <functional>
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
void writeProjectPackage(const Project& project, const QString& path,
                         const std::function<bool()>& isCancelled = {},
                         const QString& finalTargetPath = {});
void materializeProjectMedia(Project& project, const QString& directory,
                             const std::function<bool()>& isCancelled = {});
// Two-phase media materialization: stages copies/exports off the destination directory on a worker,
// then moves them into targetDirectory only after GUI-thread authorization succeeds.
struct StagedProjectMedia {
    QMap<QString, QString> assetFileNames; // asset id -> file name inside stagingDirectory
    QMap<QString, bool> extractedAssets;   // asset id -> came from packageDirectory
};
struct PublishedMediaRollback {
    QStringList movedFiles;
    QString targetDirectory;
    bool directoryCreated = false;
    void rollback() const;
};
StagedProjectMedia stageProjectMedia(const Project& project, const QString& stagingDirectory,
                                     const std::function<bool()>& isCancelled = {});
PublishedMediaRollback publishStagedProjectMedia(Project& project, const QString& stagingDirectory,
                                                 const QString& targetDirectory, const StagedProjectMedia& staged);
// Removes package extractions abandoned by crashed instances; call once at startup.
void removeStalePackageDirectories();
QString timecode(Frame position, double fps = 24);
// Whole seconds remaining, rounded up: M:SS, or H:MM:SS from an hour.
QString countdown(Frame duration);
Envelope* envelope(Clip& clip, int property);
// Sets a clip's timing; a left trim (start and offset moving together) shifts its automation.
void retime(Clip& clip, Frame start, Frame duration, Frame offset);
// Retimes a clip and applies the same start/duration/offset change to clips linked with it.
void retimeLinked(Project& project, const std::string& id, Frame start, Frame duration, Frame offset);
// The clip followed by its linked partners.
std::vector<std::string> linkedClips(const Project& project, const std::string& id);
// Splits a clip and any linked partner at timeline frame time, preserving automation across the cut.
// When rejectShapedCut is true, refuses cuts strictly inside shaped Curve/SCurve segments whose shape would change.
// Returns the primary right-half clip ID.
std::string splitLinked(Project& project, const std::string& id, Frame time, const std::string& tailId = {}, bool rejectShapedCut = false);
const Envelope* envelope(const Clip& clip, int property);
double propertyValue(const Clip& clip, int property);
// Valid automation values for a property (the same ranges as its base value).
std::pair<double, double> propertyRange(int property);
// A keyframe at time carrying the value and segment shape there, for cutting a curve.
Keyframe cutPoint(const Envelope& envelope, Frame time, double fallback);
QString shapeName(CurveShape shape);
}
