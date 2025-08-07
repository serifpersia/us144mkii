#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "alsacontroller.h"
#include <QMap>
#include <QPixmap>
#include <QWidget>

class QLabel;
class QComboBox;
class QPushButton;
class QDialog;

class MainWindow : public QWidget {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  void initUi();
  void loadDynamicSettings();
  std::pair<QWidget *, QComboBox *>
  createControlWidget(const QString &labelText, const QStringList &items);
  void updateCombo(QComboBox *combo, const std::string &controlName);

private slots:
  void onControlChanged(const std::string &controlName, int index,
                        QComboBox *combo);
  void showAboutDialog();

private:
  AlsaController m_alsa;
  QPixmap m_background;

  QMap<QString, QLabel *> m_infoLabels;
  QComboBox *m_capture12Combo;
  QComboBox *m_capture34Combo;
  QComboBox *m_lineOutCombo;
  QComboBox *m_digitalOutCombo;
  QDialog *m_aboutDialog;
};

#endif