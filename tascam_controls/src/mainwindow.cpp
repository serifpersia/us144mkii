#include "mainwindow.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPainter>
#include <QDebug>
#include <QTimer>

#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QIcon>

const QString DARK_STYLESHEET = R"(
    QWidget {
        background-color: transparent;
        color: #DAE0ED;
        font-family: Arial;
    }
    QLabel {
        background-color: transparent;
    }
    QLabel#Title {
        font-size: 15pt;
        font-weight: bold;
        color: #FFFFFF;
    }
    QLabel#SectionHeader {
        font-size: 11pt;
        font-weight: bold;
        color: #92E8FF;
        margin-top: 10px;
        margin-bottom: 3px;
    }
    QLabel#ControlLabel {
        font-size: 9pt;
        color: #CBD2E6;
    }
    QComboBox {
        background-color: rgba(10, 10, 20, 0.25);
        border: 1px solid #3A4760;
        border-radius: 4px;
        padding: 4px;
        color: #DAE0ED;
    }
    QComboBox:hover {
        background-color: rgba(15, 15, 25, 0.35);
        border: 1px solid #6482B4;
    }
    QComboBox::drop-down {
        border: none;
    }
    QComboBox QAbstractItemView {
        background-color: rgba(15, 15, 25, 0.9);
        border: 1px solid #3A4760;
        selection-background-color: #6A3AB1;
        color: #DAE0ED;
    }
    QPushButton {
        background-color: rgba(10, 10, 20, 0.25);
        border: 1px solid #3A4760;
        border-radius: 4px;
        padding: 5px;
        color: #92E8FF;
    }
    QPushButton:hover {
        background-color: rgba(15, 15, 25, 0.35);
        border: 1px solid #6482B4;
    }
    QPushButton:pressed {
        background-color: rgba(20, 20, 30, 0.45);
        border: 1px solid #A020F0;
    }
)";

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
    , m_alsa()
    , m_aboutDialog(nullptr)
{
    if (!m_alsa.isCardFound()) {
        QMessageBox::critical(this, "Error", "TASCAM US-144/US-144MKII Not Found.\nPlease ensure the device is connected and the 'us144mkii' driver is loaded.");
        QTimer::singleShot(0, this, &QWidget::close);
        return;
    }

    initUi();
    loadDynamicSettings();
}

void MainWindow::initUi() {
    setWindowTitle("TASCAM US-144MKII Control Panel");
    setWindowIcon(QIcon(":/tascam-control-panel.png"));
    setFixedSize(550, 450);
    setStyleSheet(DARK_STYLESHEET);

    m_background.load(":/bg.png");
    if (m_background.isNull()) {
        qWarning() << "Failed to load background image from resources!";
    }

    auto *topLevelLayout = new QHBoxLayout(this);
    topLevelLayout->setContentsMargins(20, 20, 20, 20);
    topLevelLayout->setSpacing(25);

    auto *leftPanel = new QVBoxLayout();
    auto *middlePanel = new QVBoxLayout();

    auto *logoLabel = new QLabel();
    logoLabel->setPixmap(QPixmap(":/logo.png").scaledToWidth(250, Qt::SmoothTransformation));
    auto *titleLabel = new QLabel("US-144 MKII Control Panel");
    titleLabel->setObjectName("Title");

    auto *infoGrid = new QGridLayout();
    infoGrid->setSpacing(5);
    const QMap<QString, QString> infoData = {
        {"Driver Version:", "driver_version"}, {"Device:", "device"},
        {"Sample Width:", "sample_width"}, {"Sample Rate:", "sample_rate"}
    };
    int row = 0;
    for (auto it = infoData.constBegin(); it != infoData.constEnd(); ++it) {
        auto *label = new QLabel(it.key());
        label->setFont(QFont("Arial", 9, QFont::Bold));
        auto *valueLabel = new QLabel("N/A");
        valueLabel->setFont(QFont("Arial", 9));
        infoGrid->addWidget(label, row, 0);
        infoGrid->addWidget(valueLabel, row, 1);
        m_infoLabels[it.value()] = valueLabel;
        row++;
    }

    auto *deviceImageLabel = new QLabel();
    deviceImageLabel->setPixmap(QPixmap(":/device.png").scaled(250, 250, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    deviceImageLabel->setAlignment(Qt::AlignCenter);

    leftPanel->addWidget(logoLabel);
    leftPanel->addWidget(titleLabel);
    leftPanel->addLayout(infoGrid);
    leftPanel->addStretch();
    leftPanel->addWidget(deviceImageLabel);

    middlePanel->setSpacing(0);

    auto addSection = [&](const QString& title, QWidget* widget) {
        auto* header = new QLabel(title);
        header->setObjectName("SectionHeader");
        middlePanel->addWidget(header);
        middlePanel->addWidget(widget);
    };

    auto capture12Pair = createControlWidget("ch1 and ch2", {"analog inputs", "digital inputs"});
    m_capture12Combo = capture12Pair.second;
    m_capture12Combo->setToolTip("Select the source for capture channels 1 and 2.");
    auto capture34Pair = createControlWidget("ch3 and ch4", {"analog inputs", "digital inputs"});
    m_capture34Combo = capture34Pair.second;
    m_capture34Combo->setToolTip("Select the source for capture channels 3 and 4.");
    addSection("INPUTS", capture12Pair.first);
    middlePanel->addWidget(capture34Pair.first);

    auto lineOutPair = createControlWidget("ch1 and ch2", {"ch1 and ch2", "ch3 and ch4"});
    m_lineOutCombo = lineOutPair.second;
    m_lineOutCombo->setToolTip("Select the source for the line outputs.");
    addSection("LINE OUTPUTS", lineOutPair.first);

    auto digitalOutPair = createControlWidget("ch3 and ch4", {"ch1 and ch2", "ch3 and ch4"});
    m_digitalOutCombo = digitalOutPair.second;
    m_digitalOutCombo->setToolTip("Select the source for the digital outputs.");
    addSection("DIGITAL OUTPUTS", digitalOutPair.first);

    middlePanel->addStretch();

    auto *buttonLayout = new QHBoxLayout();
    auto *aboutButton = new QPushButton("About");
    aboutButton->setFixedSize(100, 30);
    connect(aboutButton, &QPushButton::clicked, this, &MainWindow::showAboutDialog);
    auto *exitButton = new QPushButton("Exit");
    exitButton->setFixedSize(100, 30);
    connect(exitButton, &QPushButton::clicked, this, &QWidget::close);
    buttonLayout->addWidget(aboutButton);
    buttonLayout->addWidget(exitButton);
    middlePanel->addLayout(buttonLayout);

    topLevelLayout->addLayout(leftPanel, 1);
    topLevelLayout->addLayout(middlePanel, 1);

    connect(m_lineOutCombo, &QComboBox::currentIndexChanged, this, [this](int index){ onControlChanged("Line OUTPUTS Source", index, m_lineOutCombo); });
    connect(m_digitalOutCombo, &QComboBox::currentIndexChanged, this, [this](int index){ onControlChanged("Digital OUTPUTS Source", index, m_digitalOutCombo); });
    connect(m_capture12Combo, &QComboBox::currentIndexChanged, this, [this](int index){ onControlChanged("ch1 and ch2 Source", index, m_capture12Combo); });
    connect(m_capture34Combo, &QComboBox::currentIndexChanged, this, [this](int index){ onControlChanged("ch3 and ch4 Source", index, m_capture34Combo); });
}

void MainWindow::loadDynamicSettings() {
    m_infoLabels["driver_version"]->setText(QString::fromStdString(m_alsa.readSysfsAttr("driver_version")));
    m_infoLabels["device"]->setText("US-144 MKII");
    m_infoLabels["sample_width"]->setText("24 bits");

    long rate_val = m_alsa.getControlValue("Sample Rate");
    m_infoLabels["sample_rate"]->setText(rate_val > 0 ? QString("%1 kHz").arg(rate_val / 1000.0, 0, 'f', 1) : "N/A (inactive)");

    updateCombo(m_lineOutCombo, "Line OUTPUTS Source");
    updateCombo(m_digitalOutCombo, "Digital OUTPUTS Source");
    updateCombo(m_capture12Combo, "ch1 and ch2 Source");
    updateCombo(m_capture34Combo, "ch3 and ch4 Source");
}

void MainWindow::updateCombo(QComboBox* combo, const std::string& controlName) {
    long value = m_alsa.getControlValue(controlName);
    combo->blockSignals(true);
    combo->setCurrentIndex(static_cast<int>(value));
    combo->blockSignals(false);
}

void MainWindow::onControlChanged(const std::string& controlName, int index, QComboBox* combo) {
    m_alsa.setControlValue(controlName, index);

    if (combo) {
        QString originalStyle = combo->styleSheet();
        combo->setStyleSheet(originalStyle + " QComboBox { border: 1px solid #A020F0; }");
        QTimer::singleShot(200, [combo, originalStyle]() {
            combo->setStyleSheet(originalStyle);
        });
    }
}

std::pair<QWidget*, QComboBox*> MainWindow::createControlWidget(const QString& labelText, const QStringList& items) {
    auto *container = new QWidget();
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(2);

    auto *label = new QLabel(labelText);
    label->setObjectName("ControlLabel");
    auto *combo = new QComboBox();
    combo->addItems(items);

    layout->addWidget(label);
    layout->addWidget(combo);

    return {container, combo};
}

void MainWindow::showAboutDialog() {
    if (m_aboutDialog && m_aboutDialog->isVisible()) {
        m_aboutDialog->raise();
        m_aboutDialog->activateWindow();
        return;
    }

    if (!m_aboutDialog) {
        m_aboutDialog = new QDialog(this);
        m_aboutDialog->setWindowTitle("About TASCAM US-144MKII Control Panel");
        m_aboutDialog->setWindowIcon(QIcon(":/tascam-control-panel.png"));
        m_aboutDialog->setFixedSize(400, 250);

        auto *layout = new QVBoxLayout(m_aboutDialog);
        auto *textLabel = new QLabel(m_aboutDialog);
        textLabel->setTextFormat(Qt::RichText);
        textLabel->setOpenExternalLinks(true);
        textLabel->setText(QString("<b>TASCAM US-144MKII Control Panel</b><br>" 
                             "Driver Version: %1<br><br>" 
                             "Copyright @serifpersia 2025<br><br>" 
                             "This application provides a graphical interface to control the TASCAM US-144MKII audio interface on Linux. " 
                             "It utilizes the 'us144mkii' ALSA driver.<br><br>" 
                             "For more information, bug reports, and contributions, please visit the GitHub repository:<br>" 
                             "<a href='https://github.com/serifpersia/us144mkii'>https://github.com/serifpersia/us144mkii</a>").arg(m_infoLabels["driver_version"]->text()));
        textLabel->setWordWrap(true);

        auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(buttonBox, &QDialogButtonBox::rejected, m_aboutDialog, &QDialog::close);

        layout->addWidget(textLabel);
        layout->addWidget(buttonBox);
    }

    m_aboutDialog->show();
}

void MainWindow::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    if (!m_background.isNull()) {
        painter.drawPixmap(this->rect(), m_background);
    }
    QWidget::paintEvent(event);
}
