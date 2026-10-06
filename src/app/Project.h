#pragma once
#include "core/Model.h"
#include "media/Media.h"
#include <QMap>
#include <QString>

namespace sav {
struct Project {
    QString title = "Untitled show";
    int width = 1920, height = 1080;
    double fps = 24;
    Show show;
    QMap<QString, MediaAsset> assets;
};
QString newId();
Project newProject();
Project demoProject();
void validate(const Project& project);
void bindAudio(Project& project);
Project readProject(const QString& path);
void writeProject(const Project& project, const QString& path);
QString timecode(Frame position, double fps = 24);
Envelope* envelope(Clip& clip, int property);
const Envelope* envelope(const Clip& clip, int property);
double propertyValue(const Clip& clip, int property);
}
