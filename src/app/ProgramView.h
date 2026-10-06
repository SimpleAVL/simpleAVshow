#pragma once
#include <QWidget>
#include <QImage>

namespace sav {
class ProgramView : public QWidget {
    Q_OBJECT
public:
    explicit ProgramView(QWidget* parent = nullptr);
    void setPicture(QImage picture);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QImage picture_;
};
}
