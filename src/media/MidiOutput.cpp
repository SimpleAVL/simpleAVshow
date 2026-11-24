#include "media/MidiOutput.h"
#include <chrono>
#include <cstddef>
#include <mutex>
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <mmsystem.h>
#endif

namespace sav {
#ifdef Q_OS_MACOS
namespace {
// Apple documents that disposing an app's last CoreMIDI client can cause the MIDI server to
// shut down and subsequent MIDIClientCreate calls in the same process to fail. Keep one shared
// client for the process lifetime (matching RtMidi's design) and only dispose individual ports.
MIDIClientRef sharedCoreMidiClient(QString& error) {
    static std::mutex mutex;
    static MIDIClientRef client = 0;
    std::lock_guard lock(mutex);
    if (!client) {
        const OSStatus status = MIDIClientCreate(CFSTR("SimpleAVShow"), nullptr, nullptr, &client);
        if (status != noErr || !client) {
            client = 0;
            error = QString("Cannot create CoreMIDI client (OSStatus %1)").arg(static_cast<int>(status));
            return 0;
        }
    }
    return client;
}

QString cfStringProperty(MIDIObjectRef obj, CFStringRef prop) {
    if (!obj) return {};
    CFStringRef cf = nullptr;
    if (MIDIObjectGetStringProperty(obj, prop, &cf) != noErr || !cf) return {};
    const QString result = QString::fromCFString(cf);
    CFRelease(cf);
    return result;
}

QString coreMidiEndpointName(MIDIEndpointRef ep) {
    if (!ep) return {};
    QString name = cfStringProperty(ep, kMIDIPropertyDisplayName);
    if (name.isEmpty()) name = cfStringProperty(ep, kMIDIPropertyName);
    return name;
}

// Legacy RtMidi-style candidate names (local endpoint/device names AND connected external
// Audio MIDI Setup devices via kMIDIPropertyConnectionUniqueID) so saved preferences created
// by older RtMidi-based releases continue to resolve after upgrade.
QStringList coreMidiEndpointAliases(MIDIEndpointRef ep) {
    QStringList names;
    auto add = [&](const QString& s) { if (!s.isEmpty() && !names.contains(s)) names << s; };
    const QString display = cfStringProperty(ep, kMIDIPropertyDisplayName);
    const QString raw = cfStringProperty(ep, kMIDIPropertyName);
    add(display);
    add(raw);
    MIDIEntityRef entity = 0;
    if (MIDIEndpointGetEntity(ep, &entity) == noErr && entity) {
        MIDIDeviceRef device = 0;
        if (MIDIEntityGetDevice(entity, &device) == noErr && device) {
            const QString devName = cfStringProperty(device, kMIDIPropertyName);
            const QString entName = cfStringProperty(entity, kMIDIPropertyName);
            if (!devName.isEmpty() && !raw.isEmpty()) add(devName + " " + raw);
            if (!devName.isEmpty() && !entName.isEmpty()) add(devName + " " + entName);
        }
    }
    auto addExternalUid = [&](MIDIUniqueID uid) {
        if (!uid) return;
        MIDIObjectRef connObj = 0;
        MIDIObjectType connType = kMIDIObjectType_Other;
        if (MIDIObjectFindByUniqueID(uid, &connObj, &connType) != noErr || !connObj) return;
        add(cfStringProperty(connObj, kMIDIPropertyDisplayName));
        const QString objName = cfStringProperty(connObj, kMIDIPropertyName);
        add(objName);
        MIDIEntityRef extEntity = 0;
        if (connType == kMIDIObjectType_ExternalEntity || connType == kMIDIObjectType_Entity) {
            extEntity = static_cast<MIDIEntityRef>(connObj);
        } else if (connType == kMIDIObjectType_ExternalDestination || connType == kMIDIObjectType_ExternalSource ||
                   connType == kMIDIObjectType_Destination || connType == kMIDIObjectType_Source) {
            MIDIEndpointGetEntity(static_cast<MIDIEndpointRef>(connObj), &extEntity);
        }
        if (extEntity) {
            const QString entName = cfStringProperty(extEntity, kMIDIPropertyName);
            add(entName);
            MIDIDeviceRef extDev = 0;
            if (MIDIEntityGetDevice(extEntity, &extDev) == noErr && extDev) {
                const QString devName = cfStringProperty(extDev, kMIDIPropertyName);
                add(devName);
                if (!devName.isEmpty() && !objName.isEmpty() && !objName.startsWith(devName))
                    add(devName + " " + objName);
                if (!devName.isEmpty() && !entName.isEmpty() && !entName.startsWith(devName))
                    add(devName + " " + entName);
            }
        }
    };
    CFDataRef connData = nullptr;
    if (MIDIObjectGetDataProperty(ep, kMIDIPropertyConnectionUniqueID, &connData) == noErr && connData) {
        const CFIndex count = CFDataGetLength(connData) / static_cast<CFIndex>(sizeof(MIDIUniqueID));
        const auto* rawIds = reinterpret_cast<const SInt32*>(CFDataGetBytePtr(connData));
        for (CFIndex i = 0; i < count; ++i)
            addExternalUid(static_cast<MIDIUniqueID>(CFSwapInt32BigToHost(static_cast<uint32_t>(rawIds[i]))));
        CFRelease(connData);
    } else {
        SInt32 singleUid = 0;
        if (MIDIObjectGetIntegerProperty(ep, kMIDIPropertyConnectionUniqueID, &singleUid) == noErr && singleUid)
            addExternalUid(static_cast<MIDIUniqueID>(singleUid));
    }
    return names;
}

bool coreMidiEndpointExists(MIDIEndpointRef target) {
    if (!target) return false;
    const ItemCount count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; ++i) {
        if (MIDIGetDestination(i) == target) return true;
    }
    return false;
}
}

struct MidiOutput::CoreMidiState {
    MIDIPortRef outPort = 0;
    MIDIEndpointRef endpoint = 0;
    ~CoreMidiState() {
        if (outPort) MIDIPortDispose(outPort);
    }
    static std::unique_ptr<CoreMidiState> open(MIDIEndpointRef target, QString& error) {
        if (!target) {
            error = "MIDI port is no longer available";
            return nullptr;
        }
        const MIDIClientRef client = sharedCoreMidiClient(error);
        if (!client) return nullptr;
        auto state = std::make_unique<CoreMidiState>();
        const OSStatus status = MIDIOutputPortCreate(client, CFSTR("SimpleAVShow Output"), &state->outPort);
        if (status != noErr || !state->outPort) {
            error = QString("Cannot create CoreMIDI output port (OSStatus %1)").arg(static_cast<int>(status));
            return nullptr;
        }
        state->endpoint = target;
        return state;
    }
};
#else
struct MidiOutput::CoreMidiState {};
#endif

#ifdef Q_OS_WIN
struct MidiOutput::WinMmState {
    HMIDIOUT handle = nullptr;
    UINT devId = 0;
    ~WinMmState() {
        if (handle) {
            midiOutReset(handle);
            midiOutClose(handle);
        }
    }
    static std::unique_ptr<WinMmState> open(UINT devId, QString& error) {
        auto state = std::make_unique<WinMmState>();
        state->devId = devId;
        const MMRESULT res = midiOutOpen(&state->handle, devId, 0, 0, CALLBACK_NULL);
        if (res != MMSYSERR_NOERROR || !state->handle) {
            error = QString("Cannot open MIDI port (WinMM error %1)").arg(static_cast<unsigned>(res));
            state->handle = nullptr;
            return nullptr;
        }
        return state;
    }
};
#else
struct MidiOutput::WinMmState {};
#endif

MidiOutput::MidiOutput() = default;

MidiOutput::~MidiOutput() {
    close();
}

bool MidiOutput::connected() const {
#ifdef Q_OS_MACOS
    return coreMidi_ && coreMidi_->outPort && coreMidi_->endpoint && !lost_.load();
#elif defined(Q_OS_WIN)
    return winMm_ && winMm_->handle && !lost_.load();
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
#elif defined(Q_OS_WIN)
    const UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            result << QString::fromWCharArray(caps.szPname);
        else
            result << QString();
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
#elif defined(Q_OS_WIN)
    const UINT count = midiOutGetNumDevs();
    if (static_cast<UINT>(port) >= count) { error = "MIDI port is no longer available"; return false; }
    MIDIOUTCAPSW caps{};
    if (midiOutGetDevCapsW(static_cast<UINT>(port), &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
        error = "MIDI port is no longer available";
        return false;
    }
    const QString portName = QString::fromWCharArray(caps.szPname);
    auto state = WinMmState::open(static_cast<UINT>(port), error);
    if (!state) return false;
    winMm_ = std::move(state);
    name_ = portName;
    lost_.store(false);
    abortSend_.store(false, std::memory_order_release);
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
    // Resolve across all destinations first, ranking matches strictly (exact display name > CI display
    // name > exact alias > CI alias). If multiple endpoints tie at the best rank, keep the currently
    // connected endpoint when it is among the ties; otherwise reject the ambiguous name.
    const ItemCount count = MIDIGetNumberOfDestinations();
    struct Candidate { MIDIEndpointRef ep; QString name; };
    std::vector<Candidate> bestMatches;
    int bestRank = 99; // 0 = exact display name, 1 = CI display name, 2 = exact alias, 3 = CI alias
    for (ItemCount i = 0; i < count; ++i) {
        const MIDIEndpointRef ep = MIDIGetDestination(i);
        const QString primary = coreMidiEndpointName(ep);
        int rank = 99;
        if (primary == wanted) {
            rank = 0;
        } else if (primary.compare(wanted, Qt::CaseInsensitive) == 0) {
            rank = 1;
        } else {
            for (const auto& alias : coreMidiEndpointAliases(ep)) {
                if (alias == wanted) { rank = std::min(rank, 2); break; }
                if (alias.compare(wanted, Qt::CaseInsensitive) == 0) rank = std::min(rank, 3);
            }
        }
        if (rank < bestRank) {
            bestRank = rank;
            bestMatches.clear();
            bestMatches.push_back({ep, primary});
        } else if (rank == bestRank && rank < 99) {
            bestMatches.push_back({ep, primary});
        }
    }
    if (bestMatches.empty()) { error = "MIDI port not found: " + wanted; return false; }
    // Reuse the open connection if the currently connected endpoint is among the winning matches.
    if (connected() && coreMidiEndpointExists(coreMidi_->endpoint)) {
        for (const auto& cand : bestMatches) {
            if (cand.ep == coreMidi_->endpoint) {
                name_ = cand.name;
                return true;
            }
        }
    }
    if (bestMatches.size() > 1) {
        error = "Multiple MIDI destinations share the name “" + wanted + "”; choose the port manually in Outputs";
        return false;
    }
    const MIDIEndpointRef matchedEp = bestMatches.front().ep;
    const QString matchedName = bestMatches.front().name;
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
#elif defined(Q_OS_WIN)
    const UINT count = midiOutGetNumDevs();
    std::vector<std::pair<UINT, QString>> exactMatches, ciMatches;
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        const QString portName = QString::fromWCharArray(caps.szPname);
        if (portName == wanted) exactMatches.emplace_back(i, portName);
        else if (portName.compare(wanted, Qt::CaseInsensitive) == 0) ciMatches.emplace_back(i, portName);
    }
    const auto& matches = !exactMatches.empty() ? exactMatches : ciMatches;
    if (matches.empty()) { error = "MIDI port not found: " + wanted; return false; }
    if (connected()) {
        for (const auto& [devId, portName] : matches) {
            (void)devId;
            if (portName == name_) return true;
        }
    }
    if (matches.size() > 1) {
        error = "Multiple MIDI ports share the name “" + wanted + "”; choose the port manually in Outputs";
        return false;
    }
    const UINT matchedId = matches.front().first;
    const QString matchedName = matches.front().second;
    MIDIOUTCAPSW verifyCaps{};
    if (matchedId >= midiOutGetNumDevs() ||
        midiOutGetDevCapsW(matchedId, &verifyCaps, sizeof(verifyCaps)) != MMSYSERR_NOERROR ||
        QString::fromWCharArray(verifyCaps.szPname) != matchedName) {
        error = "MIDI port list changed before opening " + matchedName;
        return false;
    }
    auto candidate = WinMmState::open(matchedId, error);
    if (!candidate) return false;
    close();
    winMm_ = std::move(candidate);
    name_ = matchedName;
    lost_.store(false);
    abortSend_.store(false, std::memory_order_release);
    return true;
#else
    // Resolve the target port across all destinations first, preferring exact name over case-insensitive
    // match, rejecting ambiguous duplicate names, and reusing the live connection when matched.
    std::string candidateError;
    try {
        auto candidate = std::make_unique<RtMidiOut>();
        candidate->setErrorCallback([](RtMidiError::Type type, const std::string& text, void* ctx) {
            if (type != RtMidiError::DEBUG_WARNING) *static_cast<std::string*>(ctx) = text;
        }, &candidateError);
        const unsigned count = candidate->getPortCount();
        std::vector<std::pair<int, QString>> exactMatches, ciMatches;
        for (unsigned i = 0; i < count; ++i) {
            const QString portName = QString::fromStdString(candidate->getPortName(i));
            if (portName == wanted) exactMatches.emplace_back(static_cast<int>(i), portName);
            else if (portName.compare(wanted, Qt::CaseInsensitive) == 0) ciMatches.emplace_back(static_cast<int>(i), portName);
        }
        const auto& matches = !exactMatches.empty() ? exactMatches : ciMatches;
        if (matches.empty()) { error = "MIDI port not found: " + wanted; return false; }
        // On single-client drivers (such as WinMM on Windows), opening a second handle to the
        // already-connected port fails with MMSYSERR_ALLOCATED. Reuse when the connected port matches.
        if (connected()) {
            for (const auto& [idx, portName] : matches) {
                (void)idx;
                if (portName == name_) return true;
            }
        }
        if (matches.size() > 1) {
            error = "Multiple MIDI ports share the name “" + wanted + "”; choose the port manually in Outputs";
            return false;
        }
        const int matched = matches.front().first;
        const QString matchedName = matches.front().second;
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
#elif defined(Q_OS_WIN)
    if (message[0] != 0xf0 && message.size() <= 3) {
        DWORD packet = 0;
        for (std::size_t i = 0; i < message.size(); ++i)
            packet |= static_cast<DWORD>(message[i]) << (8 * i);
        const MMRESULT res = midiOutShortMsg(winMm_->handle, packet);
        if (res == MMSYSERR_NOERROR) return true;
        error = QString("MIDI cue not sent to %1 (WinMM error %2)").arg(name_).arg(static_cast<unsigned>(res));
        lost_.store(true);
        return false;
    }
    // Heap-allocate MIDIHDR + SysEx payload so if a broken driver still holds the header after
    // timeout + midiOutReset, we can safely leak the <=4 KiB buffer instead of freeing live DMA memory.
    struct SysexBuffer {
        MIDIHDR hdr{};
        std::vector<char> data;
    };
    auto sysex = std::make_unique<SysexBuffer>();
    sysex->data.assign(reinterpret_cast<const char*>(message.data()),
                       reinterpret_cast<const char*>(message.data() + message.size()));
    sysex->hdr.lpData = sysex->data.data();
    sysex->hdr.dwBufferLength = static_cast<DWORD>(sysex->data.size());
    sysex->hdr.dwBytesRecorded = sysex->hdr.dwBufferLength;
    MMRESULT res = midiOutPrepareHeader(winMm_->handle, &sysex->hdr, sizeof(MIDIHDR));
    if (res != MMSYSERR_NOERROR) {
        error = QString("MIDI SysEx prepare failed on %1 (WinMM error %2)").arg(name_).arg(static_cast<unsigned>(res));
        lost_.store(true);
        return false;
    }
    res = midiOutLongMsg(winMm_->handle, &sysex->hdr, sizeof(MIDIHDR));
    if (res != MMSYSERR_NOERROR) {
        if (midiOutUnprepareHeader(winMm_->handle, &sysex->hdr, sizeof(MIDIHDR)) == MIDIERR_STILLPLAYING)
            sysex.release();
        error = QString("MIDI SysEx not sent to %1 (WinMM error %2)").arg(name_).arg(static_cast<unsigned>(res));
        lost_.store(true);
        return false;
    }
    // Bounded 500 ms wait that also honors abortSend_ so a stalled driver can never hang midiThread_ or shutdown.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while ((res = midiOutUnprepareHeader(winMm_->handle, &sysex->hdr, sizeof(MIDIHDR))) == MIDIERR_STILLPLAYING) {
        if (abortSend_.load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= deadline) {
            midiOutReset(winMm_->handle);
            for (int tries = 0; tries < 50; ++tries) {
                res = midiOutUnprepareHeader(winMm_->handle, &sysex->hdr, sizeof(MIDIHDR));
                if (res != MIDIERR_STILLPLAYING) break;
                Sleep(1);
            }
            if (res == MIDIERR_STILLPLAYING) sysex.release();
            error = abortSend_.load(std::memory_order_acquire)
                ? QString("MIDI SysEx aborted on %1").arg(name_)
                : QString("MIDI SysEx timed out on %1").arg(name_);
            lost_.store(true);
            return false;
        }
        Sleep(1);
    }
    if (res == MMSYSERR_NOERROR) return true;
    error = QString("MIDI SysEx unprepare failed on %1 (WinMM error %2)").arg(name_).arg(static_cast<unsigned>(res));
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
    abortSend_.store(true, std::memory_order_release);
    coreMidi_.reset();
    winMm_.reset();
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
