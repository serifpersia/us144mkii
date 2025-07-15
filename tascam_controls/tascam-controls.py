import sys
import subprocess
import re
import os
from PyQt6.QtWidgets import (QApplication, QWidget, QLabel, QComboBox,
                             QPushButton, QGridLayout, QVBoxLayout, QHBoxLayout, QMessageBox)
from PyQt6.QtGui import QPixmap, QFont, QIcon, QPainter
from PyQt6.QtCore import Qt, QBuffer, QIODevice
import base64

def resource_path(relative_path):
    try:
        base_path = sys._MEIPASS
    except Exception:
        base_path = os.path.abspath(os.path.dirname(__file__))
    return os.path.join(base_path, relative_path)

DARK_STYLESHEET = """
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
"""

class AmixerController:
    @staticmethod
    def get_card_id(card_name="US144MKII"):
        try:
            output = subprocess.check_output(['aplay', '-l'], text=True)
            for line in output.splitlines():
                if card_name in line:
                    match = re.match(r'card (\d+):', line)
                    if match:
                        return match.group(1)
        except (FileNotFoundError, subprocess.CalledProcessError):
            return None
        return None

    @staticmethod
    def get_control_value(card_id, control_name):
        if not card_id: return 0
        try:
            cmd = ['amixer', '-c', card_id, 'cget', f"name='{control_name}'"]
            output = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
            for line in output.splitlines():
                if ': values=' in line:
                    return int(line.split('=')[1])
        except (FileNotFoundError, subprocess.CalledProcessError, IndexError, ValueError):
            return 0
        return 0

    @staticmethod
    def set_control_value(card_id, control_name, value):
        if not card_id: return False
        try:
            cmd = ['amixer', '-c', card_id, 'cset', f"name='{control_name}'", str(value)]
            subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return True
        except (FileNotFoundError, subprocess.CalledProcessError):
            return False

    @staticmethod
    def read_sysfs_attr(card_id, attr_name):
        path = f"/sys/class/sound/card{card_id}/device/{attr_name}"
        if os.path.exists(path):
            try:
                with open(path, 'r') as f:
                    return f.read().strip()
            except IOError:
                return "N/A"
        return "N/A"

class TascamControlPanel(QWidget):
    def __init__(self, card_id):
        super().__init__()
        self.card_id = card_id
        self.init_ui()
        self.load_dynamic_settings()

    def init_ui(self):
        self.setWindowTitle("TASCAM US-144MKII Control Panel")
        self.setWindowIcon(QIcon(resource_path("icon.ico")))
        self.setFixedSize(820, 450)

        self.setStyleSheet(DARK_STYLESHEET)

        self.background_label = QLabel(self)
        self.background_label.setGeometry(self.rect())
        self.background_label.setAlignment(Qt.AlignmentFlag.AlignCenter)

        bg_image_path = resource_path("bg.png")
        self.original_bg_pixmap = QPixmap(bg_image_path)

        if self.original_bg_pixmap.isNull():
            print(f"Warning: Could not load background image from {bg_image_path}. Using solid color.")
            self.setStyleSheet(self.styleSheet() + "TascamControlPanel { background-color: #1a1a1a; }")
        else:
            self._update_background_pixmap()

        self.background_label.lower()

        content_container = QWidget(self)
        top_level_layout = QHBoxLayout(content_container)
        top_level_layout.setContentsMargins(20, 20, 20, 20)
        top_level_layout.setSpacing(25)

        main_overall_layout = QVBoxLayout(self)
        main_overall_layout.setContentsMargins(0, 0, 0, 0)
        main_overall_layout.addWidget(content_container)

        left_panel = QVBoxLayout()
        info_grid = QGridLayout()
        logo_label = QLabel()
        logo_label.setPixmap(QPixmap(resource_path("logo.png")).scaledToWidth(250, Qt.TransformationMode.SmoothTransformation))
        title_label = QLabel("US-144 MKII Control Panel")
        title_label.setObjectName("Title")
        left_panel.addWidget(logo_label)
        left_panel.addWidget(title_label)
        info_grid.setSpacing(5)
        self.info_labels = {}
        info_data = {
            "Driver Version:": "driver_version", "Device:": "device",
            "Sample Width:": "sample_width", "Sample Rate:": "sample_rate",
            "Sample Clock Source:": "clock_source", "Digital Input Status:": "digital_status"
        }
        row = 0
        for label_text, key in info_data.items():
            label = QLabel(label_text)
            label.setFont(QFont("Arial", 9, QFont.Weight.Bold))
            value_label = QLabel("N/A")
            value_label.setFont(QFont("Arial", 9))
            info_grid.addWidget(label, row, 0)
            info_grid.addWidget(value_label, row, 1)
            self.info_labels[key] = value_label
            row += 1
        left_panel.addLayout(info_grid)
        left_panel.addStretch()

        middle_panel = QVBoxLayout()
        middle_panel.setSpacing(0)

        # --- Latency Setting Re-added ---
        latency_header = QLabel("AUDIO PERFORMANCE")
        latency_header.setObjectName("SectionHeader")
        latency_container, self.latency_combo = self.create_control_widget("Latency Profile", ["low latency", "normal latency", "high latency"])
        middle_panel.addWidget(latency_header)
        middle_panel.addWidget(latency_container)
        # --- End Latency Setting Re-added ---

        inputs_header = QLabel("INPUTS")
        inputs_header.setObjectName("SectionHeader")
        capture_12_container, self.capture_12_combo = self.create_control_widget("ch1 and ch2", ["Analog In", "Digital In"])
        capture_34_container, self.capture_34_combo = self.create_control_widget("ch3 and ch4", ["Analog In", "Digital In"])
        middle_panel.addWidget(inputs_header)
        middle_panel.addWidget(capture_12_container)
        middle_panel.addWidget(capture_34_container)

        line_header = QLabel("LINE")
        line_header.setObjectName("SectionHeader")
        line_out_container, self.line_out_combo = self.create_control_widget("ch1 and ch2", ["Playback 1-2", "Playback 3-4"])
        middle_panel.addWidget(line_header)
        middle_panel.addWidget(line_out_container)

        digital_header = QLabel("DIGITAL")
        digital_header.setObjectName("SectionHeader")
        digital_out_container, self.digital_out_combo = self.create_control_widget("ch3 and ch4", ["Playback 1-2", "Playback 3-4"])
        middle_panel.addWidget(digital_header)
        middle_panel.addWidget(digital_out_container)

        middle_panel.addStretch()

        right_panel = QVBoxLayout()
        device_image_label = QLabel()
        device_image_label.setPixmap(QPixmap(resource_path("device.png")).scaled(250, 250, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation))
        device_image_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        exit_button = QPushButton("Exit")
        exit_button.setFixedSize(100, 30)
        exit_button.clicked.connect(self.close)
        right_panel.addWidget(device_image_label)
        right_panel.addStretch()
        right_panel.addWidget(exit_button, 0, Qt.AlignmentFlag.AlignCenter)

        top_level_layout.addLayout(left_panel, 3)
        top_level_layout.addLayout(middle_panel, 3)
        top_level_layout.addLayout(right_panel, 3)

        # --- Latency Signal Connection Re-added ---
        self.latency_combo.currentIndexChanged.connect(lambda i: self.set_value("Latency Profile", i))
        # --- End Latency Signal Connection Re-added ---

        self.line_out_combo.currentIndexChanged.connect(lambda i: self.set_value("Line Out Source", i))
        self.digital_out_combo.currentIndexChanged.connect(lambda i: self.set_value("Digital Out Source", i))
        self.capture_12_combo.currentIndexChanged.connect(lambda i: self.set_value("Capture 1-2 Source", i))
        self.capture_34_combo.currentIndexChanged.connect(lambda i: self.set_value("Capture 3-4 Source", i))

    def _update_background_pixmap(self):
        if not self.original_bg_pixmap.isNull():
            scaled_pixmap = self.original_bg_pixmap.scaled(
                self.size(),
                Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                Qt.TransformationMode.SmoothTransformation
            )
            self.background_label.setPixmap(scaled_pixmap)

    def resizeEvent(self, event):
        self.background_label.setGeometry(self.rect())
        self._update_background_pixmap()
        super().resizeEvent(event)

    def create_control_widget(self, label_text, combo_items):
        container_widget = QWidget()
        layout = QVBoxLayout(container_widget)
        layout.setContentsMargins(0, 8, 0, 8)
        layout.setSpacing(2)
        label = QLabel(label_text)
        label.setObjectName("ControlLabel")
        combo_box = QComboBox()
        combo_box.addItems(combo_items)
        layout.addWidget(label)
        layout.addWidget(combo_box)
        return container_widget, combo_box

    def load_dynamic_settings(self):
        self.info_labels['driver_version'].setText(AmixerController.read_sysfs_attr(self.card_id, "driver_version"))
        self.info_labels['device'].setText("US-144 MKII")
        self.info_labels['sample_width'].setText("24 bits")
        self.info_labels['clock_source'].setText("internal")
        self.info_labels['digital_status'].setText("unavailable")

        rate_val = AmixerController.get_control_value(self.card_id, "Sample Rate")
        self.info_labels['sample_rate'].setText(f"{rate_val / 1000:.1f} kHz" if rate_val > 0 else "N/A (inactive)")

        # --- Latency Setting Load Re-added ---
        self.update_combo(self.latency_combo, "Latency Profile")
        # --- End Latency Setting Load Re-added ---

        self.update_combo(self.line_out_combo, "Line Out Source")
        self.update_combo(self.digital_out_combo, "Digital Out Source")
        self.update_combo(self.capture_12_combo, "Capture 1-2 Source")
        self.update_combo(self.capture_34_combo, "Capture 3-4 Source")

    def update_combo(self, combo, control_name):
        value = AmixerController.get_control_value(self.card_id, control_name)
        combo.blockSignals(True)
        combo.setCurrentIndex(value)
        combo.blockSignals(False)

    def set_value(self, control_name, index):
        AmixerController.set_control_value(self.card_id, control_name, index)

def main():
    app = QApplication(sys.argv)

    card_id = AmixerController.get_card_id()
    if not card_id:
        QMessageBox.critical(None, "Error", "TASCAM US-144MKII Not Found.\nPlease ensure the device is connected and the 'us144mkii' driver is loaded.")
        sys.exit(1)

    panel = TascamControlPanel(card_id)
    panel.show()
    sys.exit(app.exec())

if __name__ == '__main__':
    main()
