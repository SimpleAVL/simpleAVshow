#include "app/MidiMessageEditor.h"
#include "media/MidiOutput.h"
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <array>

namespace sav {
namespace {
constexpr std::array<int, 8> statuses = {0, 0x90, 0x80, 0xb0, 0xc0, 0xa0, 0xd0, 0xe0};
QString noteName(int number) {
    const QStringList names = {"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"};
    return names[number % 12] + QString::number(number / 12 - 1);
}
QString controlName(int number) {
    switch (number) {
    case 1: return "Modulation"; case 7: return "Volume"; case 10: return "Pan";
    case 11: return "Expression"; case 64: return "Sustain pedal"; case 91: return "Reverb";
    case 93: return "Chorus"; case 120: return "All sound off"; case 123: return "All notes off";
    default: return "Controller " + QString::number(number);
    }
}
QSpinBox* spin(const char* name, int min, int max) {
    auto* box = new QSpinBox; box->setObjectName(name); box->setRange(min, max); box->setKeyboardTracking(false); return box;
}
}
QString midiHex(const std::vector<unsigned char>& bytes) {
    QStringList list; for (auto b : bytes) list << QString("%1").arg(b, 2, 16, QChar('0')).toUpper(); return list.join(' ');
}
QString midiSummary(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return "No MIDI message";
    if (!validMidi(bytes)) return "Incomplete MIDI message";
    const QString channel = QString(" · channel %1").arg((bytes[0] & 0xf) + 1);
    switch (bytes[0] & 0xf0) {
    case 0x90: return bytes[2] == 0 ? "Stop " + noteName(bytes[1]) + channel : QString("Play %1 · velocity %2").arg(noteName(bytes[1])).arg(bytes[2]) + channel;
    case 0x80: return "Stop " + noteName(bytes[1]) + channel;
    case 0xb0: return QString("%1 → %2").arg(controlName(bytes[1])).arg(bytes[2]) + channel;
    case 0xc0: return QString("Program %1").arg(bytes[1] + 1) + channel;
    case 0xa0: return QString("%1 pressure → %2").arg(noteName(bytes[1])).arg(bytes[2]) + channel;
    case 0xd0: return QString("Channel pressure → %1").arg(bytes[1]) + channel;
    case 0xe0: return QString("Pitch bend → %1").arg((bytes[1] | (bytes[2] << 7)) - 8192) + channel;
    default: return bytes[0] == 0xf0 ? "SysEx message" : "System message · " + midiHex(bytes);
    }
}
MidiMessageEditor::MidiMessageEditor(QWidget* parent) : QWidget(parent) {
    form_ = new QFormLayout(this); form_->setContentsMargins(0, 0, 0, 0);
    type_ = new QComboBox; type_->setObjectName("midiMessageType");
    type_->addItems({"No message", "Play a note", "Stop a note", "Change a control (CC)",
        "Select a program", "Note pressure", "Channel pressure", "Pitch bend", "Raw MIDI / SysEx (advanced)"});
    channel_ = spin("midiChannel", 1, 16);
    note_ = new QComboBox; note_->setObjectName("midiNote");
    for (int n = 0; n < 128; ++n) note_->addItem(QString("%1 · %2").arg(noteName(n)).arg(n), n);
    note_->setCurrentIndex(60); note_->setToolTip("Middle C is MIDI note 60, labeled C4 here. Some devices call it C3.");
    control_ = new QComboBox; control_->setObjectName("midiController");
    for (int n = 0; n < 128; ++n) control_->addItem(QString("%1 · CC %2").arg(controlName(n)).arg(n), n);
    control_->setCurrentIndex(7);
    for (auto* box : {type_, note_, control_}) {
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(10);
    }
    value_ = spin("midiValue", 0, 127); amount_ = spin("midiAmount", 0, 127);
    raw_ = new QLineEdit; raw_->setObjectName("midiRawBytes"); raw_->setPlaceholderText("F0 7D 01 F7"); raw_->setMaxLength(12287);
    form_->addRow("Action", type_); form_->addRow("Channel", channel_); form_->addRow("Note", note_);
    form_->addRow("Control", control_); form_->addRow("Value", value_); form_->addRow("Velocity", amount_); form_->addRow("Hex bytes", raw_);
    hint_ = new QLabel; hint_->setObjectName("midiMessageHint"); hint_->setWordWrap(true); hint_->setStyleSheet("color: #88a0be; font-size: 10px;"); form_->addRow(hint_);
    preview_ = new QLabel; preview_->setObjectName("midiMessagePreview"); preview_->setWordWrap(true); preview_->setTextInteractionFlags(Qt::TextSelectableByMouse); form_->addRow(preview_);
    connect(type_, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) updateFields(true); });
    for (auto* box : {channel_, value_, amount_}) connect(box, &QSpinBox::valueChanged, this, [this] { if (!loading_) updatePreview(); });
    for (auto* box : {note_, control_}) connect(box, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) updatePreview(); });
    connect(raw_, &QLineEdit::textChanged, this, [this] { if (!loading_) updatePreview(); });
    updateFields(true);
}
void MidiMessageEditor::updateFields(bool defaults) {
    const int type = type_->currentIndex();
    loading_ = true;
    form_->setRowVisible(channel_, type > 0 && type < 8);
    form_->setRowVisible(note_, type == 1 || type == 2 || type == 5);
    form_->setRowVisible(control_, type == 3);
    form_->setRowVisible(value_, type == 3 || type == 4 || type == 6 || type == 7);
    form_->setRowVisible(amount_, type == 1 || type == 2 || type == 5);
    form_->setRowVisible(raw_, type == 8);
    value_->setRange(type == 7 ? -8192 : type == 4 ? 1 : 0, type == 7 ? 8191 : type == 4 ? 128 : 127);
    qobject_cast<QLabel*>(form_->labelForField(value_))->setText(type == 4 ? "Program" : type == 7 ? "Bend" : type == 6 ? "Pressure" : "Value");
    qobject_cast<QLabel*>(form_->labelForField(amount_))->setText(type == 5 ? "Pressure" : type == 2 ? "Release velocity" : "Velocity");
    if (defaults) { value_->setValue(type == 4 ? 1 : type == 3 ? 127 : 0); amount_->setValue(type == 2 ? 0 : 100); }
    const QStringList hints = {
        "A cue can simply pause the show without sending MIDI.",
        "Velocity is how hard the note is played: 1–127. Use Stop a note (or velocity 0) to end it.",
        "Stops a note on the selected channel. Match the note and channel used to start it.",
        "Controls a device parameter. Values run from 0 to 127; sustain is off below 64 and on from 64.",
        "Programs are shown as 1–128; MIDI sends 0–127. Check which numbering your device uses.",
        "Changes pressure for one note, from 0 to 127.",
        "Changes pressure for the whole channel, from 0 to 127.",
        "0 is centered. Negative bends down, positive bends up; the range in semitones is set on your device.",
        "Enter one complete MIDI message as hexadecimal bytes separated by spaces. SysEx begins with F0 and ends with F7."
    };
    hint_->setText(hints[type]); loading_ = false; updatePreview();
}
bool MidiMessageEditor::message(std::vector<unsigned char>& bytes, QString& error) const {
    bytes.clear(); error.clear(); const int type = type_->currentIndex();
    if (type == 8) {
        const auto tokens = raw_->text().simplified().split(' ', Qt::SkipEmptyParts);
        if (tokens.isEmpty()) { error = "Enter a complete MIDI message, for example F0 7D 01 F7."; return false; }
        for (const auto& token : tokens) {
            bool ok; const auto value = token.toUInt(&ok, 16);
            if (!ok || token.size() > 2 || value > 255) { error = "Use hexadecimal bytes separated by spaces, for example F0 7D 01 F7."; return false; }
            bytes.push_back(static_cast<unsigned char>(value));
        }
    } else if (type > 0) {
        int first = type == 1 || type == 2 || type == 5 ? note_->currentData().toInt() :
            type == 3 ? control_->currentData().toInt() : type == 4 ? value_->value() - 1 : value_->value();
        int second = type == 3 ? value_->value() : amount_->value();
        if (type == 7) { const int bend = value_->value() + 8192; first = bend & 127; second = bend >> 7; }
        bytes = {static_cast<unsigned char>(statuses[type] | (channel_->value() - 1)), static_cast<unsigned char>(first)};
        if (type != 4 && type != 6) bytes.push_back(static_cast<unsigned char>(second));
    }
    if (!validMidi(bytes)) { error = "The MIDI message is incomplete or contains invalid bytes. SysEx must end with F7."; return false; }
    return true;
}
void MidiMessageEditor::setMessage(const std::vector<unsigned char>& bytes) {
    int type = bytes.empty() ? 0 : 8;
    if (!bytes.empty() && bytes[0] < 0xf0 && validMidi(bytes)) {
        for (int i = 1; i < 8; ++i) if ((bytes[0] & 0xf0) == statuses[i]) type = i;
        // Note On with velocity 0 acts as a stop; show its exact bytes rather than a "play" with no level.
        if (type == 1 && bytes[2] == 0) type = 8;
    }
    loading_ = true; type_->setCurrentIndex(type); loading_ = false; updateFields(false);
    loading_ = true;
    // Reset hidden fields too, so selecting another cue never inherits its values.
    channel_->setValue(1); note_->setCurrentIndex(60); control_->setCurrentIndex(7);
    value_->setValue(type == 4 ? 1 : 0); amount_->setValue(100); raw_->setText(midiHex(bytes));
    if (type > 0 && type < 8) {
        channel_->setValue((bytes[0] & 0xf) + 1);
        if (type == 1 || type == 2 || type == 5) { note_->setCurrentIndex(bytes[1]); amount_->setValue(bytes[2]); }
        else if (type == 3) { control_->setCurrentIndex(bytes[1]); value_->setValue(bytes[2]); }
        else value_->setValue(type == 4 ? bytes[1] + 1 : type == 7 ? (bytes[1] | (bytes[2] << 7)) - 8192 : bytes[1]);
    }
    loading_ = false; updatePreview();
}
void MidiMessageEditor::updatePreview() {
    std::vector<unsigned char> bytes; QString error;
    const bool valid = message(bytes, error);
    preview_->setStyleSheet(valid ? "color: #9fc3ec; font-size: 11px;" : "color: #e3adb3; font-size: 11px;");
    preview_->setText(valid ? midiSummary(bytes) : error);
    preview_->setToolTip(valid ? "Bytes sent: " + midiHex(bytes) : error);
}
}
