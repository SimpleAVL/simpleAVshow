#pragma once
#include <RtMidi.h>
#include <QStringList>
#include <memory>

namespace sav {
class MidiOutput {
public:
    QStringList ports();
    bool open(int port, QString& error);
    bool send(const std::vector<unsigned char>& message, QString& error);
    void close();
    bool connected() const { return output_ && output_->isPortOpen(); }
    QString name() const { return name_; }
private:
    std::unique_ptr<RtMidiOut> output_;
    QString name_;
};
bool validMidi(const std::vector<unsigned char>& message);
}
