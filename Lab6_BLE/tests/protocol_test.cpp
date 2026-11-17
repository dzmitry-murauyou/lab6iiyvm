// ===========================================================================
//  protocol_test.cpp — автотесты протокольной части (без Bluetooth и Windows).
//
//  Собирается обычным компилятором:
//      g++ -std=c++17 -O2 tests/protocol_test.cpp -o protocol_test
//      cl /EHsc /std:c++17 tests\protocol_test.cpp
//
//  Проверяется то, что можно проверить без оборудования:
//    • CRC-32 на эталонных векторах и при потоковом расчёте;
//    • разбор строк протокола (START/READY/ACK/DONE);
//    • кадр Data: кодирование/декодирование, граничные смещения;
//    • безопасность имени принимаемого файла;
//    • модель передачи с потерями, дубликатами и возобновлением —
//      правило «принимаем блок только со своим смещением» не портит файл,
//      а CRC сходится.
// ===========================================================================
#include "../crc32.h"
#include "../protocol.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failed;
        std::printf("  [FAIL] %s\n", what);
    }
}

template <typename T>
void CheckEqual(const T& actual, const T& expected, const char* what) {
    ++g_checks;
    if (!(actual == expected)) {
        ++g_failed;
        std::printf("  [FAIL] %s\n", what);
    }
}

// ---------------------------------------------------------------------------
//  CRC-32
// ---------------------------------------------------------------------------
void TestCrc32() {
    std::puts("CRC-32");
    CheckEqual(lab6::Crc32::Compute("123456789", 9), 0xCBF43926u, "эталонный вектор");
    CheckEqual(lab6::Crc32::Compute("", 0), 0x00000000u, "пустой ввод");

    // Потоковый расчёт должен совпадать с разовым при любом разбиении.
    const std::string data = "Bluetooth LE file transfer, variant 8";
    lab6::Crc32 streaming;
    for (char ch : data) streaming.Update(&ch, 1);
    CheckEqual(streaming.Value(), lab6::Crc32::Compute(data.data(), data.size()),
               "потоковый CRC = разовый");

    // Смена одного байта меняет сумму.
    std::string changed = data;
    changed[5] = 'X';
    Check(lab6::Crc32::Compute(changed.data(), changed.size()) !=
              lab6::Crc32::Compute(data.data(), data.size()),
          "изменение байта меняет CRC");
}

// ---------------------------------------------------------------------------
//  Строки протокола
// ---------------------------------------------------------------------------
void TestProtocolStrings() {
    std::puts("Строки протокола");

    CheckEqual(ble::proto::ParseUll(L"12345"), 12345ull, "ParseUll: число");
    CheckEqual(ble::proto::ParseUll(L"123abc"), 123ull, "ParseUll: хвост");
    CheckEqual(ble::proto::ParseUll(L""), 0ull, "ParseUll: пусто");
    CheckEqual(ble::proto::ParseUll(L"18446744073709551615"), 18446744073709551615ull,
               "ParseUll: максимальное 64-битное смещение");

    {
        // START|размер|crc|имя — имя может содержать пробелы и точки.
        const auto parts = ble::proto::Split(L"START|1048576|A1B2C3D4|Отчёт 2026.pdf", L'|', 4);
        CheckEqual(parts.size(), (size_t)4, "Split: 4 части");
        Check(parts.size() == 4 && parts[0] == L"START", "Split: команда");
        Check(parts.size() == 4 && parts[1] == L"1048576", "Split: размер");
        Check(parts.size() == 4 && parts[2] == L"A1B2C3D4", "Split: crc");
        Check(parts.size() == 4 && parts[3] == L"Отчёт 2026.pdf", "Split: имя файла");
    }
    {
        // Лишние разделители не должны «съедать» имя: хвост идёт в последнюю часть.
        const auto parts = ble::proto::Split(L"DONE|100|ABCD|OK|C:\\dir\\file.bin", L'|', 6);
        CheckEqual(parts.size(), (size_t)5, "Split: DONE с путём");
        Check(parts.size() == 5 && parts[3] == L"OK", "Split: признак OK");
    }
    {
        const auto parts = ble::proto::Split(L"ACK|8192", L'|', 4);
        Check(parts.size() == 2 && parts[0] == L"ACK", "Split: ACK");
        CheckEqual(ble::proto::ParseUll(parts.size() > 1 ? parts[1] : L""), 8192ull,
                   "ACK: смещение разбирается");
    }
    {
        const auto parts = ble::proto::Split(L"", L'|', 4);
        CheckEqual(parts.size(), (size_t)1, "Split: пустая строка");
    }
}

// ---------------------------------------------------------------------------
//  Имя файла
// ---------------------------------------------------------------------------
void TestSanitize() {
    std::puts("Имя файла");
    CheckEqual(ble::proto::SanitizeName(L"photo.jpg"), std::wstring(L"photo.jpg"), "обычное имя");
    CheckEqual(ble::proto::SanitizeName(L"..\\..\\Windows\\system32\\evil.exe"),
               std::wstring(L".._.._Windows_system32_evil.exe"), "выход из каталога");
    CheckEqual(ble::proto::SanitizeName(L"a|b?.bin"), std::wstring(L"a_b_.bin"),
               "запрещённые символы");
    CheckEqual(ble::proto::SanitizeName(L""), std::wstring(L"received.bin"), "пустое имя");
    CheckEqual(ble::proto::SanitizeName(L".."), std::wstring(L"received.bin"), "точки");
    CheckEqual(ble::proto::SanitizeName(std::wstring(300, L'x')).size(), (size_t)96,
               "ограничение длины");
}

// ---------------------------------------------------------------------------
//  Кадр Data
// ---------------------------------------------------------------------------
void TestFrame() {
    std::puts("Кадр Data");

    const std::uint64_t offsets[] = {0ull, 1ull, 511ull, 65535ull, 0x7FFFFFFFull, 0xFFFFFFFEull};
    for (std::uint64_t offset : offsets) {
        std::uint8_t payload[512];
        for (int i = 0; i < 512; ++i) payload[i] = (std::uint8_t)(i * 7 + 3);

        std::vector<std::uint8_t> frame;
        ble::proto::EncodeFrame(offset, payload, sizeof(payload), frame);

        CheckEqual(frame.size(), sizeof(payload) + ble::proto::kFrameHeader, "размер кадра");

        std::uint32_t decoded_offset = 0;
        const std::uint8_t* decoded_payload = nullptr;
        std::size_t decoded_size = 0;
        Check(ble::proto::DecodeFrame(frame.data(), frame.size(), decoded_offset, decoded_payload,
                                      decoded_size),
              "кадр разбирается");
        CheckEqual<unsigned long long>(decoded_offset, offset, "смещение сохраняется");
        CheckEqual(decoded_size, sizeof(payload), "размер данных сохраняется");
        Check(decoded_payload != nullptr && std::memcmp(decoded_payload, payload, sizeof(payload)) == 0,
              "данные не искажаются");
    }

    {
        std::uint32_t offset = 0;
        const std::uint8_t* payload = nullptr;
        std::size_t size = 0;
        Check(!ble::proto::DecodeFrame(nullptr, 10, offset, payload, size), "nullptr отвергается");
        const std::uint8_t too_short[4] = {1, 2, 3, 4};
        Check(!ble::proto::DecodeFrame(too_short, 4, offset, payload, size),
              "кадр без данных отвергается");
    }
}

// ---------------------------------------------------------------------------
//  Наихудший размер блока при MTU 23: 20 байт ATT = 4 смещения + 16 данных
// ---------------------------------------------------------------------------
void TestMtuSizes() {
    std::puts("Размеры блоков");
    for (int mtu : {23, 185, 517}) {
        const int att = mtu - 3;
        const int data = att - 4;
        Check(data >= 16 || mtu == 23, "полезная нагрузка не меньше 16 байт при MTU > 23");
        Check(att <= 514, "ATT payload в пределах BLE");
    }
    CheckEqual((int)(23 - 3 - 4), 16, "MTU 23 -> 16 байт данных");
    CheckEqual((int)(517 - 3 - 4), 510, "MTU 517 -> 510 байт данных");
}

// ---------------------------------------------------------------------------
//  Модель передачи: потери, дубликаты, возобновление.
//
//  Повторяет правила ble.cpp:
//    • приёмник пишет блок только если его смещение равно текущей длине файла;
//    • иначе отбрасывает и сообщает своё смещение (ACK);
//    • отправитель откатывается к подтверждённому смещению и повторяет;
//    • при обрыве связи сеанс сохраняется — продолжение с принятой длины.
//  Используются настоящие EncodeFrame/DecodeFrame и CRC-32.
// ---------------------------------------------------------------------------
struct ModelReceiver {
    std::vector<std::uint8_t> file;
    unsigned long long received = 0;

    // Возвращает true, если блок принят.
    bool OnData(const std::vector<std::uint8_t>& frame) {
        std::uint32_t offset = 0;
        const std::uint8_t* payload = nullptr;
        std::size_t size = 0;
        if (!ble::proto::DecodeFrame(frame.data(), frame.size(), offset, payload, size)) return false;
        if (offset != received) return false;   // дубликат/пропуск — не пишем
        file.insert(file.end(), payload, payload + size);
        received += size;
        return true;
    }
};

struct TransferResult {
    bool crc_ok = false;
    unsigned long long delivered = 0;
    unsigned long long blocks = 0;
    unsigned long long retries = 0;
};

TransferResult RunTransfer(const std::vector<std::uint8_t>& source, const std::uint32_t source_crc,
                           int mtu, double loss_probability, double duplicate_probability,
                           unsigned long long disconnect_at, std::mt19937& rng) {
    const int chunk = mtu - 3 - 4;
    std::uniform_real_distribution<double> chance(0.0, 1.0);

    ModelReceiver receiver;
    TransferResult result;

    unsigned long long cursor = 0;
    bool disconnected = false;

    while (cursor < source.size()) {
        // «Обрыв связи»: обнуляем прогресс отправителя, но не приёмника.
        if (!disconnected && disconnect_at > 0 && cursor >= disconnect_at) {
            disconnected = true;
            cursor = receiver.received;   // после переподключения узнаём смещение из READY
        }

        const std::size_t size = std::min<std::size_t>((std::size_t)chunk, source.size() - cursor);
        std::vector<std::uint8_t> frame;
        ble::proto::EncodeFrame(cursor, source.data() + cursor, size, frame);

        // Потеря блока в буфере контроллера.
        if (chance(rng) < loss_probability) {
            ++result.retries;
            // Отправитель узнаёт подтверждённое смещение и откатывается.
            cursor = receiver.received;
            continue;
        }
        // Дубликат: тот же блок приходит ещё раз.
        if (chance(rng) < duplicate_probability) {
            receiver.OnData(frame);
        }

        if (receiver.OnData(frame)) {
            cursor += size;
            ++result.blocks;
        } else {
            ++result.retries;
            cursor = receiver.received;
        }
    }

    result.delivered = receiver.received;
    result.crc_ok = receiver.file == source &&
                    lab6::Crc32::Compute(receiver.file.data(), receiver.file.size()) == source_crc;
    return result;
}

void TestTransferModel() {
    std::puts("Модель передачи");

    std::mt19937 rng(20261117);

    // Файл ~300 КБ с повторяющимся, но не тривиальным содержимым.
    std::vector<std::uint8_t> source(300 * 1024);
    for (size_t i = 0; i < source.size(); ++i) source[i] = (std::uint8_t)((i * 31 + (i >> 8)) & 0xFF);
    const std::uint32_t crc = lab6::Crc32::Compute(source.data(), source.size());

    for (int mtu : {23, 185, 517}) {
        // 1. Идеальные условия.
        auto clean = RunTransfer(source, crc, mtu, 0.0, 0.0, 0, rng);
        Check(clean.crc_ok, "чистая передача: CRC совпал");
        CheckEqual(clean.delivered, (unsigned long long)source.size(), "чистая передача: все байты");

        // 2. 5 % потерь.
        auto lossy = RunTransfer(source, crc, mtu, 0.05, 0.0, 0, rng);
        Check(lossy.crc_ok, "5% потерь: файл не испорчен");
        CheckEqual(lossy.delivered, (unsigned long long)source.size(), "5% потерь: все байты");

        // 3. 3 % дубликатов.
        auto doubles = RunTransfer(source, crc, mtu, 0.0, 0.03, 0, rng);
        Check(doubles.crc_ok, "дубликаты: файл не испорчен");

        // 4. Обрыв на середине + возобновление.
        auto resumed = RunTransfer(source, crc, mtu, 0.0, 0.0, source.size() / 2, rng);
        Check(resumed.crc_ok, "возобновление: CRC совпал");
        CheckEqual(resumed.delivered, (unsigned long long)source.size(),
                   "возобновление: файл собран целиком");

        // 5. Обрыв + потери одновременно.
        auto both = RunTransfer(source, crc, mtu, 0.05, 0.02, source.size() / 3, rng);
        Check(both.crc_ok, "обрыв + потери: файл не испорчен");
    }

    // Маленький файл (меньше одного ACK-интервала в 8 КБ).
    std::vector<std::uint8_t> tiny(300);
    for (size_t i = 0; i < tiny.size(); ++i) tiny[i] = (std::uint8_t)i;
    const std::uint32_t tiny_crc = lab6::Crc32::Compute(tiny.data(), tiny.size());
    auto small = RunTransfer(tiny, tiny_crc, 517, 0.0, 0.0, 0, rng);
    Check(small.crc_ok, "маленький файл: CRC совпал");
    CheckEqual(small.delivered, (unsigned long long)tiny.size(), "маленький файл: все байты");
}

}  // namespace

int main() {
    TestCrc32();
    TestProtocolStrings();
    TestSanitize();
    TestFrame();
    TestMtuSizes();
    TestTransferModel();

    std::printf("\nПроверок: %d, ошибок: %d\n", g_checks, g_failed);
    std::puts(g_failed == 0 ? "РЕЗУЛЬТАТ: OK" : "РЕЗУЛЬТАТ: ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
