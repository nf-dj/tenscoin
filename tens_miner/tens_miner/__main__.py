"""Entry point for TensCoin miner GUI."""

import sys
from PyQt6.QtWidgets import QApplication
from tens_miner.ui import MainWindow  # Use absolute import

def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())

if __name__ == '__main__':
    main()