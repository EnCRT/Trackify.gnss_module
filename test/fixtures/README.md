# Trackify Binary Log Fixtures

Эталонные `.bin` файлы для тестирования парсеров (Flutter, Kotlin, Swift, Python).
По одной версии формата — один файл.

## Файлы

| Файл | VER | Размер | Записей | Особенности |
|------|-----|--------|---------|-------------|
| `v1_sample.bin` | 1 | 837 B | 20 | Без блока метаданных, записи сразу после `\n` |
| `v2_sample.bin` | 2 | 901 B | 20 | LogMeta 64B между заголовком и записями |
| `../../log_ternovka_4laps.bin` | 2 | 550 KB | 14 467 | Полноразмерный заезд (4 круга по 2.3–2.5 мин) в г. Терновка для E2E тестов и эмулятора |

## Генерация

```bash
# Микро-фикстуры (для юнит-тестов парсеров)
python test/generate_fixtures.py          # регенерировать все
python test/generate_fixtures.py --v1     # только VER=1
python test/generate_fixtures.py --v2     # только VER=2

# Полноразмерная гоночная сессия (4 круга, 25 Гц, для эмулятора и UI анализа)
python generate_ternovka_bin.py
```

## Структура трека

Оба файла содержат один и тот же синтетический трек:
- Круговая траектория диаметром ~200 м
- Центр: 48.536°N, 9.051°E
- Скорость: 15 м/с (54 км/ч)
- Частота: 25 Гц
- Спутники: 16–19
- Тип фикса: 3D

## Использование в тестах

### Python
```python
data = open("test/fixtures/v2_sample.bin", "rb").read()
nl = data.find(b"\n")
header = data[:nl].decode()
assert "VER=2" in header
meta = data[nl+1:nl+65]   # 64 bytes
records_start = nl + 65
record = struct.unpack("<IiiiiiiIIBB", data[records_start:records_start+38])
```

### Dart/Flutter
```dart
final bytes = await rootBundle.load('assets/fixtures/v2_sample.bin');
final headerEnd = bytes.indexOf(0x0A);
final meta = LogMeta(ByteData.sublistView(bytes), headerEnd + 1);
final recordStart = headerEnd + 1 + 64;
final record = TrackRecord(ByteData.sublistView(bytes), recordStart);
```

### Добавление новой версии

1. Добавить `write_meta_vN()` в `test/generate_fixtures.py`
2. Добавить `generate_vN()` с нужными параметрами
3. Запустить `python test/generate_fixtures.py`
4. Обновить эту таблицу
