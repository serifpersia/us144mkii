import sys
import subprocess
import re
import os
from PyQt6.QtWidgets import (QApplication, QWidget, QLabel, QComboBox,
                             QPushButton, QGridLayout, QVBoxLayout, QHBoxLayout, QMessageBox)
from PyQt6.QtGui import QPixmap, QFont, QIcon
from PyQt6.QtCore import Qt

def resource_path(relative_path):
    try:
        base_path = sys._MEIPASS
    except Exception:
        base_path = os.path.abspath(os.path.dirname(__file__))
    return os.path.join(base_path, relative_path)

DARK_STYLESHEET = """
QWidget {
    background-color: #2b2b2b;
    color: #f0f0f0;
    font-family: Arial;
}
QLabel {
    background-color: transparent;
}
QComboBox {
    background-color: #3c3f41;
    border: 1px solid #555;
    border-radius: 4px;
    padding: 4px;
}
QComboBox:hover {
    border: 1px solid #777;
}
QComboBox::drop-down {
    border: none;
}
QComboBox QAbstractItemView {
    background-color: #3c3f41;
    border: 1px solid #555;
    selection-background-color: #5a5d5f;
}
QPushButton {
    background-color: #3c3f41;
    border: 1px solid #555;
    border-radius: 4px;
    padding: 5px;
}
QPushButton:hover {
    background-color: #4f5254;
}
QPushButton:pressed {
    background-color: #5a5d5f;
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
    def get_control_string(card_id, control_name):
        if not card_id: return "N/A"
        try:
            cmd = ['amixer', '-c', card_id, 'cget', f"name='{control_name}'"]
            output = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
            for line in output.splitlines():
                if ': values=' in line:
                    value_str = line.split('=')[1]
                    byte_values = [int(b, 16) for b in value_str.split(',')]
                    return bytes(byte_values).partition(b'\0')[0].decode('utf-8', errors='ignore').strip()
        except (FileNotFoundError, subprocess.CalledProcessError, IndexError, ValueError):
            return "Error"
        return "N/A"

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
        self.setFixedSize(800, 450)
        self.setStyleSheet(DARK_STYLESHEET)

        main_layout = QHBoxLayout(self)
        left_panel, middle_panel, right_panel = QVBoxLayout(), QVBoxLayout(), QVBoxLayout()

        header_layout = QVBoxLayout()
        logo_label = QLabel()
        logo_label.setPixmap(QPixmap(resource_path("logo.png")).scaled(250, 50, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation))
        title_label = QLabel("US-144 MKII Control Panel")
        title_label.setFont(QFont("Arial", 15, QFont.Weight.Bold))
        header_layout.addWidget(logo_label)
        header_layout.addWidget(title_label)

        info_layout = QGridLayout()
        self.info_labels = {}
        info_data = {
            "Driver Version:": "N/A", "Device:": "US-144 MKII",
            "Sample Width:": "24 bits", "Sample Rate:": "N/A",
            "Sample Clock Source:": "internal", "Digital Input Status:": "unavailable"
        }
        for row, (label_text, value_text) in enumerate(info_data.items()):
            key = label_text.replace(":", "").replace(" ", "_").lower()
            label = QLabel(label_text, font=QFont("Arial", 10, QFont.Weight.Bold))
            value_label = QLabel(value_text, font=QFont("Arial", 10))
            info_layout.addWidget(label, row, 0, Qt.AlignmentFlag.AlignLeft)
            info_layout.addWidget(value_label, row, 1, Qt.AlignmentFlag.AlignLeft)
            self.info_labels[key] = value_label

        left_panel.addLayout(info_layout)
        left_panel.addStretch()

        middle_panel.setSpacing(15)
        latency_container, self.latency_combo = self.create_control_widget("Audio Performance", ["Low", "Normal", "High"])
        mock_container1, _ = self.create_control_widget("Sample Clock Source", ["Internal", "Auto"])
        mock_container2, _ = self.create_control_widget("Digital Output Format", ["S/PDIF"])
        routing_container, self.routing_combo = self.create_control_widget("LINE OUTPUTS", ["Stereo to All", "Swapped", "Digital In to All"])

        middle_panel.addWidget(latency_container)
        middle_panel.addWidget(mock_container1)
        middle_panel.addWidget(mock_container2)
        middle_panel.addWidget(routing_container)
        middle_panel.addStretch()

        device_image_label = QLabel()
        device_image_label.setPixmap(QPixmap(resource_path("device.png")).scaled(250, 250, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation))
        device_image_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        exit_button = QPushButton("Exit")
        exit_button.setFixedSize(100, 30)
        exit_button.clicked.connect(self.close)
        right_panel.addWidget(device_image_label)
        right_panel.addStretch()
        right_panel.addWidget(exit_button, 0, Qt.AlignmentFlag.AlignCenter)

        top_level_layout = QVBoxLayout()
        top_level_layout.addLayout(header_layout)
        top_level_layout.addSpacing(20)
        panels_layout = QHBoxLayout()
        panels_layout.setContentsMargins(10, 0, 10, 0)
        panels_layout.addLayout(left_panel, 1)
        panels_layout.addLayout(middle_panel, 1)
        panels_layout.addLayout(right_panel, 1)
        top_level_layout.addLayout(panels_layout)
        main_layout.addLayout(top_level_layout)
        self.setLayout(main_layout)

        self.latency_combo.currentIndexChanged.connect(self.on_latency_changed)
        self.routing_combo.currentIndexChanged.connect(self.on_routing_changed)

    def create_control_widget(self, label_text, combo_items):
        container_widget = QWidget()
        layout = QVBoxLayout(container_widget)
        layout.setContentsMargins(0,0,0,0)
        layout.setSpacing(2)
        label = QLabel(label_text, font=QFont("Arial", 10, QFont.Weight.Bold))
        combo_box = QComboBox()
        combo_box.addItems(combo_items)
        layout.addWidget(label)
        layout.addWidget(combo_box)
        return container_widget, combo_box

    def load_dynamic_settings(self):
        driver_ver = AmixerController.read_sysfs_attr(self.card_id, "driver_version")
        self.info_labels['driver_version'].setText(driver_ver)

        rate_val = AmixerController.get_control_value(self.card_id, "Sample Rate")
        if rate_val > 0:
            self.info_labels['sample_rate'].setText(f"{rate_val / 1000:.1f} kHz")
        else:
            self.info_labels['sample_rate'].setText("N/A (inactive)")

        latency_val = AmixerController.get_control_value(self.card_id, "Latency Profile")
        self.latency_combo.blockSignals(True)
        self.latency_combo.setCurrentIndex(latency_val)
        self.latency_combo.blockSignals(False)

        routing_val = AmixerController.get_control_value(self.card_id, "Playback Routing")
        self.routing_combo.blockSignals(True)
        self.routing_combo.setCurrentIndex(routing_val)
        self.routing_combo.blockSignals(False)

    def on_latency_changed(self, index):
        AmixerController.set_control_value(self.card_id, "Latency Profile", index)

    def on_routing_changed(self, index):
        AmixerController.set_control_value(self.card_id, "Playback Routing", index)

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
