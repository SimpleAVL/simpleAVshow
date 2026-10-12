#pragma once
#include <RtMidi.h>
#include <QStringList>
#include <atomic>
#include <memory>
#include <string>

namespace sav {
class MidiOutput {
public:
    QStringList ports();
    bool open(int port, QString& error);
    bool send(const std::vector<unsigned char>& message, QString& error);
    void close();
    // A failed send marks the port lost; the controller reconnects it by name.
    bool connected() const { return output_ && output_->isPortOpen() && !lost_.load(); }
    QString name() const { return name_; }
private:
    std::unique_ptr<RtMidiOut> output_;
    QString name_;
    std::string error_; // Written by RtMidi on the thread using output_.
    std::atomic<bool> lost_{false};
    static void onError(RtMidiError::Type type, const std::string& text, void* self);
};
bool validMidi(const std::vector<unsigned char>& message);
}
