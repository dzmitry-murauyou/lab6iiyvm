// ===========================================================================
//  protocol.h — чистые (без Win32/WinRT) примитивы протокола передачи.
//
//  Здесь только то, что можно проверить автотестами без Bluetooth:
//    • разбор текстовых строк Control/Status (Split, ParseUll);
//    • безопасное имя принимаемого файла (SanitizeName);
//    • кадр характеристики Data: [uint32 offset][данные] (EncodeFrame/DecodeFrame).
//
//  Файл сознательно не зависит от Windows: tests/protocol_test.cpp собирается
//  обычным компилятором и проверяет эти функции.
// ===========================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ble {
namespace proto {

// Смещение в кадре — 32 бита, поэтому файл ограничен 4 ГиБ.
constexpr unsigned long long kMaxFileSize = 0xFFFFFFFFull;
constexpr std::size_t kFrameHeader = 4;

// Разбор беззнакового числа из начала строки: "12345" -> 12345,
// "123abc" -> 123, "" -> 0. Используется для "READY|смещение".
inline unsigned long long ParseUll(const std::wstring& text) {
    unsigned long long value = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') break;
        value = value * 10 + (unsigned long long)(ch - L'0');
    }
    return value;
}

// Деление строки по разделителю с ограничением числа частей:
// Split(L"START|10|AB|имя", L'|', 4) -> {"START", "10", "AB", "имя"}.
// Если разделителей больше, хвост целиком попадает в последнюю часть.
inline std::vector<std::wstring> Split(const std::wstring& text, wchar_t sep, std::size_t limit) {
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find(sep, start);
        if (pos == std::wstring::npos || (limit && parts.size() + 1 >= limit)) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

// Имя файла от удалённой стороны: убираем разделители пути и запрещённые
// в Windows символы, ограничиваем длину. Пустое имя заменяется на received.bin.
inline std::wstring SanitizeName(std::wstring name) {
    for (wchar_t& ch : name) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' ||
            ch == L'<' || ch == L'>' || ch == L'|' || ch < 32) {
            ch = L'_';
        }
    }
    if (name.empty() || name == L"." || name == L"..") name = L"received.bin";
    if (name.size() > 96) name = name.substr(0, 96);
    return name;
}

// Кадр Data: 4 байта смещения (little-endian) + данные блока.
inline void EncodeFrame(unsigned long long offset, const std::uint8_t* data, std::size_t size,
                        std::vector<std::uint8_t>& out) {
    out.resize(size + kFrameHeader);
    out[0] = (std::uint8_t)(offset & 0xFF);
    out[1] = (std::uint8_t)((offset >> 8) & 0xFF);
    out[2] = (std::uint8_t)((offset >> 16) & 0xFF);
    out[3] = (std::uint8_t)((offset >> 24) & 0xFF);
    if (size) std::memcpy(out.data() + kFrameHeader, data, size);
}

// Обратный разбор. Возвращает false, если кадр короче заголовка.
inline bool DecodeFrame(const std::uint8_t* frame, std::size_t size, std::uint32_t& offset,
                        const std::uint8_t*& payload, std::size_t& payload_size) {
    if (!frame || size < kFrameHeader + 1) return false;
    offset = (std::uint32_t)frame[0] | ((std::uint32_t)frame[1] << 8) |
             ((std::uint32_t)frame[2] << 16) | ((std::uint32_t)frame[3] << 24);
    payload = frame + kFrameHeader;
    payload_size = size - kFrameHeader;
    return true;
}

}  // namespace proto
}  // namespace ble
