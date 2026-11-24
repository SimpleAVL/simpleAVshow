#include "media/MidiOutput.h"
#include <cstddef>
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#endif

namespace sav {
#ifdef Q_OS_MACOS
struct MidiOutput::CoreMidiState {
    MIDIClientRef client = 0;
    MIDIPortRef outPort = 0;
    MIDIEndpointRef endpoint = 0;
    ~CoreMidiState() {
        if (outPort) MIDIPortDispose(outPort);
        if (client) MIDIClientDispose(client);
    }
    static std::unique_ptr<CoreMidiState> open(MIDIEndpointRef target, QString& error) {
        if (!target) {
            error = "MIDI port is no longer available";
            return nullptr;
        }
        auto state = std::make_unique<CoreMidiState>();
        OSStatus status = MIDIClientCreate(CFSTR("SimpleAVShow"), nullptr, nullptr, &state->client);
        if (status != noErr || !state->client) {
            error = QString("Cannot create CoreMIDI client (OSStatus %1)").arg(static_cast<int>(status));
            return nullptr;
        }
        status = MIDIOutputPortCreate(state->client, CFSTR("SimpleAVShow Output"), &state->outPort);
        if (status != noErr || !state->outPort) {
            error = QString("Cannot create CoreMIDI output port (OSStatus %1)").arg(static_cast<int>(status));
            return nullptr;
        }
        state->endpoint = target;
        return state;
    }
};

namespace {
QString coreMidiEndpointName(MIDIEndpointRef ep) {
    if (!ep) return {};
    CFStringRef cfName = nullptr;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cfName) != noErr || !cfName) {
        MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &cfName);
    }
    QString result;
    if (cfName) {
        result = QString::fromCFString(cfName);
        CFRelease(cfName);
    }
    return result;
}
}
#else
struct MidiOutput::CoreMidiState {};
#endif

MidiOutput::MidiOutput() = default;

MidiOutput::~MidiOutput() {
    close();
}

bool MidiOutput::connected() const {
#ifdef Q_OS_MACOS
    return coreMidi_ && coreMidi_->outPort && coreMidi_->endpoint && !lost_.load();
#else
    return output_ && output_->isPortOpen() && !lost_.load();
#endif
}

QStringList MidiOutput::ports() {
    QStringList result;
#ifdef Q_OS_MACOS
    const ItemCount count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; ++i) {
        result << coreMidiEndpointName(MIDIGetDestination(i));
    }
#else
    try {
        RtMidiOut probe;
        for (unsigned i = 0; i < probe.getPortCount(); ++i) result << QString::fromStdString(probe.getPortName(i));
    } catch (const RtMidiError&) {}
#endif
    return result;
}

void MidiOutput::onError(RtMidiError::Type type, const std::string& text, void* self) {
    if (type != RtMidiError::DEBUG_WARNING) static_cast<MidiOutput*>(self)->error_ = text;
}

bool MidiOutput::open(int port, QString& error) {
    close();
    if (port < 0) return true;
#ifdef Q_OS_MACOS
    const ItemCount count = MIDIGetNumberOfDestinations();
    if (static_cast<ItemCount>(port) >= count) { error = "MIDI port is no longer available"; return false; }
    const MIDIEndpointRef ep = MIDIGetDestination(static_cast<ItemCount>(port));
    const QString epName = coreMidiEndpointName(ep);
    auto state = CoreMidiState::open(ep, error);
    if (!state) return false;
    coreMidi_ = std::move(state);
    name_ = epName;
    lost_.store(false);
    return true;
#else
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
#endif
}

bool MidiOutput::openByName(const QString& wanted, QString& error) {
#ifdef Q_OS_MACOS
    // Resolve directly to a stable CoreMIDI MIDIEndpointRef and open a candidate client
    // before touching the existing connection so hot-plug index shifts cannot redirect cues.
    const ItemCount count = MIDIGetNumberOfDestinations();
    MIDIEndpointRef matchedEp = 0;
    QString matchedName;
    for (ItemCount i = 0; i < count; ++i) {
        const MIDIEndpointRef ep = MIDIGetDestination(i);
        const QString epName = coreMidiEndpointName(ep);
        if (epName == wanted) { matchedEp = ep; matchedName = epName; break; }
        if (!matchedEp && epName.compare(wanted, Qt::CaseInsensitive) == 0) { matchedEp = ep; matchedName = epName; }
    }
    if (!matchedEp) { error = "MIDI port not found: " + wanted; return false; }
    // Verify the endpoint's own display name directly on the MIDIEndpointRef before binding.
    if (coreMidiEndpointName(matchedEp) != matchedName) {
        error = "MIDI destination changed while opening " + matchedName;
        return false;
    }
    auto candidate = CoreMidiState::open(matchedEp, error);
    if (!candidate) return false;
    close();
    coreMidi_ = std::move(candidate);
    name_ = matchedName;
    lost_.store(false);
    return true;
#else
    // Open the replacement client before touching the existing connection so a failure preserves it.
    std::string candidateError;
    try {
        auto candidate = std::make_unique<RtMidiOut>();
        candidate->setErrorCallback([](RtMidiError::Type type, const std::string& text, void* ctx) {
            if (type != RtMidiError::DEBUG_WARNING) *static_cast<std::string*>(ctx) = text;
        }, &candidateError);
        const unsigned count = candidate->getPortCount();
        int matched = -1;
        QString matchedName;
        for (unsigned i = 0; i < count; ++i) {
            const QString portName = QString::fromStdString(candidate->getPortName(i));
            if (portName == wanted) { matched = static_cast<int>(i); matchedName = portName; break; }
            if (matched < 0 && portName.compare(wanted, Qt::CaseInsensitive) == 0) { matched = static_cast<int>(i); matchedName = portName; }
        }
        if (matched < 0) { error = "MIDI port not found: " + wanted; return false; }
        if (static_cast<unsigned>(matched) >= candidate->getPortCount() ||
            QString::fromStdString(candidate->getPortName(static_cast<unsigned>(matched))) != matchedName) {
            error = "MIDI port list changed before opening " + matchedName;
            return false;
        }
        candidate->openPort(static_cast<unsigned>(matched), "SimpleAVShow");
        // Re-verify port list stability immediately after openPort for backends with live index lookup.
        if (!candidateError.empty() || !candidate->isPortOpen() ||
            static_cast<unsigned>(matched) >= candidate->getPortCount() ||
            QString::fromStdString(candidate->getPortName(static_cast<unsigned>(matched))) != matchedName) {
            error = !candidateError.empty() ? QString::fromStdString(candidateError)
                : (!candidate->isPortOpen() ? QString("Cannot open MIDI port")
                                            : QString("MIDI port list changed while opening %1").arg(matchedName));
            return false;
        }
        close();
        output_ = std::move(candidate);
        output_->setErrorCallback(&MidiOutput::onError, this);
        name_ = matchedName;
        return true;
    } catch (const RtMidiError& e) { error = QString::fromStdString(e.getMessage()); return false; }
#endif
}

bool MidiOutput::send(const std::vector<unsigned char>& message, QString& error) {
    if (message.empty()) return true;
    if (!connected()) { error = "MIDI cue not sent: no output port is connected"; return false; }
    if (!validMidi(message)) { error = "Invalid MIDI message"; return false; }
#ifdef Q_OS_MACOS
    alignas(MIDIPacketList) std::byte storage[4096 + 256];
    auto* packetList = reinterpret_cast<MIDIPacketList*>(storage);
    MIDIPacket* packet = MIDIPacketListInit(packetList);
    packet = MIDIPacketListAdd(packetList, sizeof(storage), packet, 0, message.size(),
        reinterpret_cast<const Byte*>(message.data()));
    if (!packet) {
        error = "MIDI message exceeds CoreMIDI packet buffer";
        return false;
    }
    const OSStatus status = MIDISend(coreMidi_->outPort, coreMidi_->endpoint, packetList);
    if (status == noErr) return true;
    error = QString("MIDI cue not sent to %1 (OSStatus %2)").arg(name_).arg(static_cast<int>(status));
    lost_.store(true);
    return false;
#else
    try {
        error_.clear(); output_->sendMessage(&message);
        if (error_.empty()) return true;
        error = "MIDI cue not sent to " + name_ + ": " + QString::fromStdString(error_);
    } catch (const RtMidiError& e) { error = QString::fromStdString(e.getMessage()); }
    lost_.store(true); return false;
#endif
}

void MidiOutput::close() {
    coreMidi_.reset();
    output_.reset();
    name_.clear();
    error_.clear();
    lost_.store(false);
}

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
