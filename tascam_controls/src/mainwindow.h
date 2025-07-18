#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QPixmap>
#include <QMap>
#include "alsacontroller.h"

class QLabel;
class QComboBox;
class QPushButton;

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void initUi();
    void loadDynamicSettings();
    std::pair<QWidget*, QComboBox*> createControlWidget(const QString& labelText, const QStringList& items);
    void updateCombo(QComboBox* combo, const std::string& controlName);

private slots:
    void onControlChanged(const std::string& controlName, int index);

private:
    AlsaController m_alsa;
    QPixmap m_background;

    QMap<QString, QLabel*> m_infoLabels;
    QComboBox* m_latencyCombo;
    QComboBox* m_capture12Combo;
    QComboBox* m_capture34Combo;
    QComboBox* m_lineOutCombo;
    QComboBox* m_digitalOutCombo;
};

#endif
