#!/usr/bin/env python3
"""
ESP32 BME280 Data Logger (PRO Edition)
Парсит данные с ESP32, восстанавливает пропущенную историю и логирует в CSV/TXT.
    By Gemini 3.1 Pro
    Bug fix: Claude Sonnet 5 Medium (01.08.2026 20:24)
    От: 01.08.2026 20:24
    https://github.com/FireTIA/esp32-bme280-weather-station
    MIT License
"""

import json
import os
import sys
import time
import csv
import requests
from datetime import datetime, timezone, timedelta
from pathlib import Path

# ======================== CONFIG ========================
SETTINGS_FILE = "settings.json"

# Цвета для консоли
C_RESET = "\033[0m"
C_GREEN = "\033[92m"
C_YELLOW = "\033[93m"
C_RED = "\033[91m"
C_CYAN = "\033[96m"
C_GRAY = "\033[90m"

def load_settings(path: str = SETTINGS_FILE) -> dict:
    """Загрузка настроек из JSON-файла."""
    if not os.path.exists(path):
        print(f"{C_RED}[ERROR] Файл настроек '{path}' не найден.{C_RESET}")
        print("Создаём стандартный settings.json...")
        default_settings = {
            "esp_ip": "192.168.<заполните>.<заполните>",
            "esp_port": 80,
            "api_status": "/api/status",
            "api_history": "/api/history",
            "poll_interval_sec": 60,
            "log_dir": "./logs",
            "csv_filename": "bme280_log.csv",
            "txt_filename": "bme280_log.txt",
            "console_output": True,
            "timeout_sec": 5,
            "max_retries": 3,
            "retry_delay_sec": 10,
            # ФИКС: должно совпадать с NTP_UTC_OFFSET_SEC/3600 в прошивке ESP32
            # (по умолчанию в .ino стоит 3 = UTC+3, Москва). Нужно, чтобы
            # правильно сопоставлять base_timestamp (в TZ прошивки) с
            # локальным временем машины, на которой крутится логгер —
            # раньше при их несовпадении бэкофилл истории сдвигался на
            # разницу поясов.
            "esp_utc_offset_hours": 3
        }
        with open(path, "w", encoding="utf-8") as f:
            json.dump(default_settings, f, indent=2, ensure_ascii=False)
        return default_settings

    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

# ======================== LOGGERS ========================
class Logger:
    """Запись данных в CSV, TXT и консоль с поддержкой произвольного времени."""

    CSV_HEADER = ["Дата", "Время", "Температура (°C)", "Влажность (%)", "Давление (hPa)", "Wi-Fi SSID", "Wi-Fi RSSI (dBm)", "Wi-Fi канал"]

    def __init__(self, log_dir: str, csv_name: str, txt_name: str, console: bool = True):
        self.log_dir = Path(log_dir)
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.csv_path = self.log_dir / csv_name
        self.txt_path = self.log_dir / txt_name
        self.console = console

        # Создаём CSV-файл с заголовком, если его нет
        if not self.csv_path.exists():
            with open(self.csv_path, "w", newline="", encoding="utf-8") as f:
                writer = csv.writer(f, delimiter=";")
                writer.writerow(self.CSV_HEADER)

    def get_last_timestamp(self) -> datetime | None:
        """Считывает последнюю запись из CSV для синхронизации истории."""
        if not self.csv_path.exists():
            return None
        try:
            with open(self.csv_path, "r", encoding="utf-8") as f:
                lines = f.readlines()
                if len(lines) <= 1: # Только заголовок или пусто
                    return None
                
                # Ищем последнюю непустую строку
                for line in reversed(lines):
                    line = line.strip()
                    if not line: continue
                    
                    parts = line.split(";")
                    if len(parts) >= 2:
                        # Формат: YYYY-MM-DD HH:MM:SS
                        dt_str = f"{parts[0]} {parts[1]}"
                        return datetime.strptime(dt_str, "%Y-%m-%d %H:%M:%S")
        except Exception as e:
            print(f"{C_RED}[WARN] Ошибка чтения последнего времени из CSV: {e}{C_RESET}")
        return None

    def _format_line(self, data: dict, dt: datetime, is_history: bool = False) -> str:
        """Форматирование строки для TXT и консоли."""
        date_str = dt.strftime("%Y-%m-%d")
        time_str = dt.strftime("%H:%M:%S")
        
        # Парсинг значений с защитой от None и строк
        try: t = f"{float(data.get('t', data.get('temperature', 0))):.2f}"
        except: t = "—"
        try: h = f"{float(data.get('h', data.get('humidity', 0))):.2f}"
        except: h = "—"
        try: p = f"{float(data.get('p', data.get('pressure', 0))):.2f}"
        except: p = "—"

        wifi = data.get("wifi", {})
        ssid = wifi.get("ssid", "—") or "—"
        rssi = wifi.get("rssi", "—")
        ch = wifi.get("channel", "—")

        prefix = f"{C_CYAN}[HISTORY]{C_RESET}" if is_history else f"{C_GREEN}[LIVE]{C_RESET}"
        wifi_str = f"Wi-Fi: {ssid} (RSSI: {rssi} dBm, CH: {ch})" if not is_history else "Wi-Fi: данные из истории"

        return (
            f"{prefix} [{date_str} {time_str}] "
            f"Темп: {t:>6} °C | "
            f"Влаж: {h:>6} % | "
            f"Давл: {p:>7} hPa | "
            f"{wifi_str}"
        )

    def write(self, data: dict, dt: datetime = None, is_history: bool = False):
        """Запись одной записи во все источники."""
        if dt is None:
            dt = datetime.now()
            
        date_str = dt.strftime("%Y-%m-%d")
        time_str = dt.strftime("%H:%M:%S")

        t = data.get("t", data.get("temperature", ""))
        h = data.get("h", data.get("humidity", ""))
        p = data.get("p", data.get("pressure", ""))
        
        wifi = data.get("wifi", {})
        ssid = wifi.get("ssid", "—") or "—"
        rssi = wifi.get("rssi", "—")
        ch = wifi.get("channel", "—")

        # === CSV ===
        with open(self.csv_path, "a", newline="", encoding="utf-8") as f:
            writer = csv.writer(f, delimiter=";")
            writer.writerow([date_str, time_str, t, h, p, ssid, rssi, ch])

        # === TXT ===
        # Для TXT удаляем ANSI-коды цветов
        line_colored = self._format_line(data, dt, is_history)
        line_clean = line_colored.replace(C_GREEN, "").replace(C_CYAN, "").replace(C_RESET, "")
        
        with open(self.txt_path, "a", encoding="utf-8") as f:
            f.write(line_clean + "\n")

        # === Консоль ===
        if self.console:
            print(line_colored)

# ======================== FETCHER ========================
def fetch_api(settings: dict, endpoint: str) -> dict | None:
    """Универсальная функция для GET-запросов к API."""
    ip = settings["esp_ip"]
    port = settings.get("esp_port", 80)
    timeout = settings.get("timeout_sec", 5)
    max_retries = settings.get("max_retries", 3)
    retry_delay = settings.get("retry_delay_sec", 10)

    url = f"http://{ip}:{port}{endpoint}"

    for attempt in range(1, max_retries + 1):
        try:
            resp = requests.get(url, timeout=timeout)
            resp.raise_for_status()
            return resp.json()
        except requests.exceptions.ConnectionError:
            print(f"{C_YELLOW}[WARN] Попытка {attempt}/{max_retries}: нет соединения с {ip}{C_RESET}")
        except requests.exceptions.Timeout:
            print(f"{C_YELLOW}[WARN] Попытка {attempt}/{max_retries}: таймаут {endpoint}{C_RESET}")
        except requests.exceptions.HTTPError as e:
            print(f"{C_RED}[ERROR] HTTP ошибка {endpoint}: {e}{C_RESET}")
            return None
        except json.JSONDecodeError:
            print(f"{C_RED}[ERROR] Мусор вместо JSON от {endpoint}{C_RESET}")
            return None

        if attempt < max_retries:
            time.sleep(retry_delay)

    return None

# ======================== SYNC ENGINE ========================
def sync_history(logger: Logger, settings: dict):
    """Восстанавливает пропущенные записи из памяти ESP32."""
    print(f"{C_GRAY}>>> Проверка пропущенных данных в памяти датчика...{C_RESET}")
    
    last_csv_dt = logger.get_last_timestamp()
    if not last_csv_dt:
        print(f"{C_GRAY}>>> Лог пуст или не найден. Скачиваем всю доступную историю.{C_RESET}")
        last_csv_dt = datetime.min

    status_data = fetch_api(settings, settings.get("api_status", "/api/status"))
    history_data = fetch_api(settings, settings.get("api_history", "/api/history"))

    if not history_data or "points" not in history_data or not history_data["points"]:
        print(f"{C_GRAY}>>> История на устройстве пуста или недоступна.{C_RESET}")
        return

    points = history_data["points"]
    base_ts = history_data.get("base_timestamp", 0)
    interval = history_data.get("interval_sec", 60)

    # ФИКС: явный сдвиг часового пояса прошивки ESP32 (например UTC+3), чтобы
    # корректно сопоставлять его время с локальным (наивным) временем машины
    # логгера. Раньше datetime.fromtimestamp() интерпретировал epoch в TZ
    # операционной системы логгера, что при несовпадении поясов сдвигало
    # весь восстановленный бэкофилл на разницу между ними.
    esp_offset = timedelta(hours=settings.get("esp_utc_offset_hours", 3))

    # Корректировка времени. Если ESP32 не успела синхронизировать NTP,
    # base_timestamp будет маленьким числом (аптайм, отсчитанным от 0).
    # В этом случае epoch на ESP32 не определён вовсе, поэтому привязываемся
    # к текущему времени ПК (уже в его локальной TZ) и не применяем esp_offset,
    # т.к. base_ts в этом случае — не epoch, а просто "секунды с момента старта".
    if base_ts < 1000000000 and status_data and "uptime_sec" in status_data:
        current_pc_dt = datetime.now()
        esp_uptime = status_data["uptime_sec"]
        # секунд "назад" от текущего момента до самой старой точки в буфере
        seconds_ago = esp_uptime - base_ts
        base_dt_naive = current_pc_dt - timedelta(seconds=seconds_ago)
    else:
        # base_ts — настоящий UTC epoch с NTP. Переводим в UTC-aware datetime,
        # прибавляем offset прошивки, затем убираем tzinfo, чтобы получить
        # "наивное" локальное время в той же TZ, что и остальной CSV/консоль.
        base_dt_utc = datetime.fromtimestamp(base_ts, tz=timezone.utc)
        base_dt_naive = (base_dt_utc + esp_offset).replace(tzinfo=None)

    recovered_count = 0
    for i, pt in enumerate(points):
        # Точное время конкретной точки
        point_dt = base_dt_naive + timedelta(seconds=i * interval)

        # Если эта точка новее, чем последняя запись в CSV (плюс 1 секунда запаса на погрешность)
        if (point_dt - last_csv_dt).total_seconds() > 1:
            logger.write(pt, dt=point_dt, is_history=True)
            recovered_count += 1

    if recovered_count > 0:
        print(f"{C_GREEN}>>> Успешно восстановлено {recovered_count} пропущенных записей!{C_RESET}")
    else:
        print(f"{C_GRAY}>>> Все данные актуальны. Пропусков нет.{C_RESET}")


# ======================== MAIN ========================
def main():
    print(f"{C_CYAN}=" * 60)
    print("ESP32 BME280 Data Logger (PRO Edition)")
    print("=" * 60 + C_RESET)

    settings = load_settings()
    logger = Logger(
        log_dir=settings.get("log_dir", "./logs"),
        csv_name=settings.get("csv_filename", "bme280_log.csv"),
        txt_name=settings.get("txt_filename", "bme280_log.txt"),
        console=settings.get("console_output", True),
    )

    # 1. Синхронизируем пропуски перед основным циклом
    sync_history(logger, settings)

    # 2. Запускаем стандартный поллинг
    interval = settings.get("poll_interval_sec", 60)
    print(f"\n{C_GRAY}[INFO] Интервал опроса: {interval} сек")
    print(f"[INFO] Логи сохраняются в: {Path(settings.get('log_dir', './logs')).resolve()}")
    print(f"[INFO] Запуск цикла опроса... (Ctrl+C для остановки){C_RESET}\n")

    try:
        while True:
            data = fetch_api(settings, settings.get("api_status", "/api/status"))
            
            if data and data.get("temp") is not None:
                # Адаптируем ключи, так как /api/status отдает temp/hum/press, 
                # а в write мы ожидаем t/h/p или temperature/humidity/pressure
                formatted_data = {
                    "t": data.get("temp"),
                    "h": data.get("hum"),
                    "p": data.get("press"),
                    "wifi": {
                        "ssid": data.get("wifi_ssid"),
                        "rssi": data.get("wifi_rssi_dbm"),
                        "channel": data.get("wifi_channel")
                    }
                }
                logger.write(formatted_data, is_history=False)
            else:
                dt_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                print(f"{C_YELLOW}[{dt_str}] [SKIP] Данные не получены (возможно отвалился датчик), запись пропущена.{C_RESET}")

            time.sleep(interval)

    except KeyboardInterrupt:
        print(f"\n{C_GRAY}[INFO] Остановка по Ctrl+C. До свидания!{C_RESET}")
    except Exception as e:
        print(f"\n{C_RED}[FATAL] Неожиданная ошибка: {e}{C_RESET}")
        sys.exit(1)

if __name__ == "__main__":
    main()