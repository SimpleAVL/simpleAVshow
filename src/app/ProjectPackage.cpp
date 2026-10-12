#include "app/Project.h"
#include <archive.h>
#include <archive_entry.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

namespace sav {
namespace {
constexpr qint64 chunkSize = 1024 * 1024;
void checkArchive(archive* a, int result) {
    if (result != ARCHIVE_OK) {
        const char* error = archive_error_string(a);
        throw std::runtime_error(error ? error : "Invalid or unreadable show package");
    }
}
la_ssize_t writeBytes(archive* a, void* context, const void* data, size_t size) {
    auto& file = *static_cast<QSaveFile*>(context);
    const auto count = file.write(static_cast<const char*>(data), static_cast<qint64>(size));
    if (count != static_cast<qint64>(size)) {
        archive_set_error(a, EIO, "Cannot write package: %s", file.errorString().toUtf8().constData());
        return -1;
    }
    return static_cast<la_ssize_t>(count);
}
struct ReadContext {
    QFile file;
    QByteArray buffer = QByteArray(static_cast<int>(chunkSize), '\0');
};
la_ssize_t readBytes(archive* a, void* context, const void** data) {
    auto& input = *static_cast<ReadContext*>(context);
    const auto count = input.file.read(input.buffer.data(), input.buffer.size());
    *data = input.buffer.constData();
    if (count < 0) archive_set_error(a, EIO, "Cannot read package: %s", input.file.errorString().toUtf8().constData());
    return static_cast<la_ssize_t>(count);
}
la_int64_t seekBytes(archive* a, void* context, la_int64_t offset, int whence) {
    auto& file = static_cast<ReadContext*>(context)->file;
    const qint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? file.pos() : file.size();
    if ((offset > 0 && base > std::numeric_limits<qint64>::max() - offset) ||
        (offset < 0 && offset < -base) || !file.seek(base + offset)) {
        archive_set_error(a, EIO, "Cannot seek in package"); return ARCHIVE_FATAL;
    }
    return file.pos();
}
void addFile(archive* writer, const QString& name, const QString& source) {
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read media: " + source.toStdString() + ": " + input.errorString().toStdString());
    const auto size = input.size();
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
    if (!entry) throw std::bad_alloc();
    archive_entry_set_pathname(entry.get(), name.toUtf8().constData());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), size);
    checkArchive(writer, archive_write_header(writer, entry.get()));
    QByteArray buffer(static_cast<int>(chunkSize), '\0');
    qint64 remaining = size;
    while (remaining > 0) {
        const auto count = input.read(buffer.data(), std::min(remaining, chunkSize));
        if (count <= 0) throw std::runtime_error("Media could not be read completely: " + source.toStdString());
        qint64 written = 0;
        while (written < count) {
            const auto result = archive_write_data(writer, buffer.constData() + written, static_cast<size_t>(count - written));
            if (result <= 0) checkArchive(writer, ARCHIVE_FATAL);
            written += result;
        }
        remaining -= count;
    }
    if (input.size() != size) throw std::runtime_error("Media changed during package export: " + source.toStdString());
    checkArchive(writer, archive_write_finish_entry(writer));
}
bool safeMediaPath(const QString& path) {
    static const QRegularExpression allowed("\\Amedia/[A-Za-z0-9_-]+(?:\\.[A-Za-z0-9]{1,16})?\\z");
    return path.size() <= 255 && allowed.match(path).hasMatch();
}
}

bool isProjectPackage(const QString& path) { return path.endsWith(".savpack", Qt::CaseInsensitive); }

void writeProjectPackage(const Project& project, const QString& path) {
    validate(project);
    QTemporaryDir staging(QDir::tempPath() + "/simpleavshow-export-XXXXXX");
    if (!staging.isValid()) throw std::runtime_error("Cannot create package staging directory");
    Project candidate = project;
    // Existing sources stream directly into the archive; only generated media needs staging.
    candidate.packageDirectory.reset();
    materializeProjectMedia(candidate, staging.filePath("generated"));
    QMap<QString, QString> names;
    QMap<QString, QString> sources;
    const auto target = QFileInfo(path).absoluteFilePath();
    const auto targetCanonical = QFileInfo(path).canonicalFilePath();
    for (auto& asset : candidate.assets) {
        const QFileInfo source(asset.path);
        const auto canonical = source.canonicalFilePath();
        if (!source.isFile() || canonical.isEmpty()) throw std::runtime_error("Media file is missing: " + asset.path.toStdString());
        if (source.absoluteFilePath() == target || (!targetCanonical.isEmpty() && canonical == targetCanonical))
            throw std::runtime_error("Package destination would overwrite a media source");
        if (!names.contains(canonical)) {
            QString extension = source.suffix().toLower();
            static const QRegularExpression suffix("\\A[a-z0-9]{1,16}\\z");
            if (!suffix.match(extension).hasMatch()) extension = "bin";
            const auto name = "media/" + newId() + "." + extension;
            names.insert(canonical, name); sources.insert(name, asset.path);
        }
        asset.path = staging.filePath(names.value(canonical));
    }
    const auto manifest = staging.filePath("project.savshow");
    writeProject(candidate, manifest);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error(output.errorString().toStdString());
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    if (!writer) throw std::bad_alloc();
    checkArchive(writer.get(), archive_write_set_format_zip(writer.get()));
    // Media is generally compressed already. Store it without another expensive compression pass.
    // libarchive adds Zip64 automatically for sources/packages exceeding 4 GiB.
    checkArchive(writer.get(), archive_write_zip_set_compression_store(writer.get()));
    checkArchive(writer.get(), archive_write_set_bytes_in_last_block(writer.get(), 1));
    checkArchive(writer.get(), archive_write_open(writer.get(), &output, nullptr, writeBytes, nullptr));
    addFile(writer.get(), "project.savshow", manifest);
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) addFile(writer.get(), it.key(), it.value());
    checkArchive(writer.get(), archive_write_close(writer.get()));
    if (!output.commit()) throw std::runtime_error(output.errorString().toStdString());
}

Project readProjectPackage(const QString& path) {
    auto directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/simpleavshow-package-XXXXXX");
    if (!directory->isValid() || !QDir().mkpath(directory->filePath("media"))) throw std::runtime_error("Cannot create package working directory");
    ReadContext input;
    input.file.setFileName(path);
    if (!input.file.open(QIODevice::ReadOnly)) throw std::runtime_error(input.file.errorString().toStdString());
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    if (!reader) throw std::bad_alloc();
    checkArchive(reader.get(), archive_read_support_format_zip_seekable(reader.get()));
    checkArchive(reader.get(), archive_read_set_seek_callback(reader.get(), seekBytes));
    checkArchive(reader.get(), archive_read_open(reader.get(), &input, nullptr, readBytes, nullptr));
    QSet<QString> files, foldedNames;
    archive_entry* entry = nullptr;
    for (;;) {
        const auto result = archive_read_next_header(reader.get(), &entry);
        if (result == ARCHIVE_EOF) break;
        checkArchive(reader.get(), result);
        const char* rawName = archive_entry_pathname(entry);
        const QString name = rawName ? QString::fromUtf8(rawName) : QString();
        if ((name != "project.savshow" && !safeMediaPath(name)) || foldedNames.contains(name.toLower()) ||
            archive_entry_filetype(entry) != AE_IFREG || archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry) || !archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0)
            throw std::runtime_error("Unsafe or duplicate package entry: " + name.toStdString());
        foldedNames.insert(name.toLower()); files.insert(name);
        QFile output(directory->filePath(name));
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw std::runtime_error(output.errorString().toStdString());
        qint64 remaining = archive_entry_size(entry);
        QByteArray buffer(static_cast<int>(chunkSize), '\0');
        for (;;) {
            const auto count = archive_read_data(reader.get(), buffer.data(), static_cast<size_t>(buffer.size()));
            if (count < 0) checkArchive(reader.get(), ARCHIVE_FATAL);
            if (count == 0) break;
            if (count > remaining) throw std::runtime_error("Package entry exceeds its declared size");
            if (output.write(buffer.constData(), count) != count) throw std::runtime_error(output.errorString().toStdString());
            remaining -= count;
        }
        if (remaining != 0 || !output.flush()) throw std::runtime_error("Incomplete package entry: " + name.toStdString());
    }
    checkArchive(reader.get(), archive_read_close(reader.get()));
    if (!files.contains("project.savshow")) throw std::runtime_error("Package is missing project.savshow");
    const auto manifest = directory->filePath("project.savshow");
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || !doc.object()["assets"].isArray()) throw std::runtime_error("Invalid package project JSON");
    // Validate every reference before the regular project reader can access any files.
    for (const auto& value : doc.object()["assets"].toArray()) {
        const auto source = value.toObject()["path"].toString();
        if (!safeMediaPath(source) || !files.contains(source)) throw std::runtime_error("Package references missing or external media: " + source.toStdString());
    }
    Project project = readProject(manifest);
    project.packageDirectory = std::move(directory);
    return project;
}
}
