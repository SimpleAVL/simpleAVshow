#include "media/MidiOutput.h"

namespace sav {
QStringList MidiOutput::ports() {
    QStringList result;
    try {
        RtMidiOut probe;
        for (unsigned i = 0; i < probe.getPortCount(); ++i) result << QString::fromStdString(probe.getPortName(i));
    } catch (const RtMidiError&) {}
    return result;
}
void MidiOutput::onError(RtMidiError::Type type, const std::string& text, void* self) {
    if (type != RtMidiError::DEBUG_WARNING) static_cast<MidiOutput*>(self)->error_ = text;
}
bool MidiOutput::open(int port, QString& error) {
    close(); if (port < 0) return true;
    try {
        output_ = std::make_unique<RtMidiOut>();
        // Without a callback RtMidi only prints send warnings, so a vanished device would fail silently.
        output_->setErrorCallback(&MidiOutput::onError, this);
        if (static_cast<unsigned>(port) >= output_->getPortCount()) { error = "MIDI port is no longer available"; close(); return false; }
        name_ = QString::fromStdString(output_->getPortName(port));
        output_->openPort(port, "SimpleAVShow");
        if (!error_.empty() || !output_->isPortOpen()) { error = error_.empty() ? "Cannot open MIDI port" : QString::fromStdString(error_); close(); return false; }
        return true;
    } catch (const RtMidiError& e) { error = QString::fromStdString(e.getMessage()); close(); return false; }
}
bool MidiOutput::send(const std::vector<unsigned char>& message, QString& error) {
    if (message.empty()) return true;
    if (!connected()) { error = "MIDI cue not sent: no output port is connected"; return false; }
    if (!validMidi(message)) { error = "Invalid MIDI message"; return false; }
    try {
        error_.clear(); output_->sendMessage(&message);
        if (error_.empty()) return true;
        error = "MIDI cue not sent to " + name_ + ": " + QString::fromStdString(error_);
    } catch (const RtMidiError& e) { error = QString::fromStdString(e.getMessage()); }
    lost_.store(true); return false;
}
void MidiOutput::close() { output_.reset(); name_.clear(); error_.clear(); lost_.store(false); }
bool validMidi(const std::vector<unsigned char>& message) {
    if (message.empty()) return true;
    if (message.size() > 4096 || message[0] < 0x80) return false;
    const unsigned status = message[0];
    if (status == 0xf0) {
        if (message.size() < 2 || message.back() != 0xf7) return false;
        for (std::size_t i = 1; i + 1 < message.size(); ++i) if (message[i] >= 0x80) return false;
        return true;
    }
    std::size_t length = 0;
    if (status < 0xf0) length = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 2 : 3;
    else if (status == 0xf1 || status == 0xf3) length = 2;
    else if (status == 0xf2) length = 3;
    else if (status == 0xf6 || status == 0xf8 || status == 0xfa || status == 0xfb || status == 0xfc || status == 0xfe || status == 0xff) length = 1;
    if (message.size() != length) return false;
    for (std::size_t i = 1; i < length; ++i) if (message[i] >= 0x80) return false;
    return true;
}
}
