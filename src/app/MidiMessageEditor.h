#pragma once
#include <QWidget>
#include <vector>

class QComboBox; class QSpinBox; class QLineEdit; class QLabel; class QFormLayout;
namespace sav {
QString midiHex(const std::vector<unsigned char>& bytes);
QString midiSummary(const std::vector<unsigned char>& bytes);

// Presents musical values; the project continues to store standard MIDI bytes.
class MidiMessageEditor : public QWidget {
    Q_OBJECT
public:
    explicit MidiMessageEditor(QWidget* parent = nullptr);
    void setMessage(const std::vector<unsigned char>& bytes);
    bool message(std::vector<unsigned char>& bytes, QString& error) const;
private:
    QFormLayout* form_;
    QComboBox *type_, *note_, *control_;
    QSpinBox *channel_, *value_, *amount_;
    QLineEdit* raw_;
    QLabel *hint_, *preview_;
    bool loading_ = false;
    void updateFields(bool defaults);
    void updatePreview();
};
}
