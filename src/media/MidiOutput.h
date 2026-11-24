#pragma once
#include <RtMidi.h>
#include <QStringList>
#include <atomic>
#include <memory>
#include <string>

namespace sav {
class MidiOutput {
public:
    MidiOutput();
    ~MidiOutput();
    MidiOutput(const MidiOutput&) = delete;
    MidiOutput& operator=(const MidiOutput&) = delete;

    QStringList ports();
    bool open(int port, QString& error);
    // Resolves and opens a port by name (binding directly to the endpoint identity on CoreMIDI).
    bool openByName(const QString& name, QString& error);
    bool send(const std::vector<unsigned char>& message, QString& error);
    void close();
    // A failed send marks the port lost; the controller reconnects it by name.
    bool connected() const;
    QString name() const { return name_; }
private:
    struct CoreMidiState;
    std::unique_ptr<CoreMidiState> coreMidi_;
    std::unique_ptr<RtMidiOut> output_;
    QString name_;
    std::string error_; // Written by RtMidi on the thread using output_.
    std::atomic<bool> lost_{false};
    static void onError(RtMidiError::Type type, const std::string& text, void* self);
};
bool validMidi(const std::vector<unsigned char>& message);
}
