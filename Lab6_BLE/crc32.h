// ===========================================================================
//  crc32.h — контрольная сумма CRC-32 (IEEE 802.3, полином 0xEDB88320).
//
//  Используется в протоколе передачи файла: отправитель считает CRC всего
//  файла до передачи, приёмник — после приёма уже записанного на диск файла.
//  Суммы должны совпасть, иначе передача считается неуспешной.
//
//  Класс инкрементальный: можно считать CRC по частям (потоково), не держа
//  файл целиком в памяти. Таблица строится на этапе компиляции (C++17).
// ===========================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace lab6 {

namespace crc_detail {

constexpr std::uint32_t Entry(std::uint32_t index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    return value;
}

struct Table {
    std::uint32_t v[256]{};
    constexpr Table() {
        for (std::uint32_t i = 0; i < 256; ++i) v[i] = Entry(i);
    }
};

constexpr Table kTable{};

}  // namespace crc_detail

class Crc32 {
public:
    Crc32() = default;

    void Reset() { value_ = 0xFFFFFFFFu; }

    void Update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        std::uint32_t value = value_;
        for (std::size_t i = 0; i < size; ++i) {
            value = crc_detail::kTable.v[(value ^ bytes[i]) & 0xFFu] ^ (value >> 8);
        }
        value_ = value;
    }

    // Итоговая сумма: инверсия накопленного регистра.
    std::uint32_t Value() const { return value_ ^ 0xFFFFFFFFu; }

    static std::uint32_t Compute(const void* data, std::size_t size) {
        Crc32 crc;
        crc.Update(data, size);
        return crc.Value();
    }

private:
    std::uint32_t value_ = 0xFFFFFFFFu;
};

}  // namespace lab6
