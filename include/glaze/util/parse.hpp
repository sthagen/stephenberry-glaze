// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <bit>
#include <cstring>
#include <iterator>
#include <span>
#include <locale>

#include "glaze/api/name.hpp"
#include "glaze/core/context.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/util/expected.hpp"
#include "glaze/util/inline.hpp"
#include "glaze/util/stoui64.hpp"
#include "glaze/util/string_literal.hpp"
#include "glaze/util/string_view.hpp"

namespace glz::detail
{
   // assumes null terminated
   template <char c>
   GLZ_ALWAYS_INLINE void match(is_context auto&& ctx, auto&& it, auto&&) noexcept
   {
      if (*it != c) [[unlikely]] {
         ctx.error = error_code::syntax_error;
      }
      else [[likely]] {
         ++it;
      }
   }

   template <string_literal str>
   GLZ_ALWAYS_INLINE void match(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      const auto n = size_t(end - it);
      if ((n < str.size()) || std::memcmp(&*it, str.value, str.size())) [[unlikely]] {
         ctx.error = error_code::syntax_error;
      }
      else [[likely]] {
         it += str.size();
      }
   }

   GLZ_ALWAYS_INLINE void skip_comment(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      if (bool(ctx.error)) [[unlikely]] {
         return;
      }

      ++it;
      if (it == end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
      }
      else if (*it == '/') {
         while (++it != end && *it != '\n')
            ;
      }
      else if (*it == '*') {
         while (++it != end) {
            if (*it == '*') [[unlikely]] {
               if (++it == end) [[unlikely]]
                  break;
               else if (*it == '/') [[likely]] {
                  ++it;
                  break;
               }
            }
         }
      }
      else [[unlikely]] {
         ctx.error = error_code::expected_end_comment;
      }
   }

   consteval uint64_t repeat_byte(char c)
   {
      const auto byte = uint8_t(c);
      uint64_t res{};
      res |= uint64_t(byte) << 56;
      res |= uint64_t(byte) << 48;
      res |= uint64_t(byte) << 40;
      res |= uint64_t(byte) << 32;
      res |= uint64_t(byte) << 24;
      res |= uint64_t(byte) << 16;
      res |= uint64_t(byte) << 8;
      res |= uint64_t(byte);
      return res;
   }

   GLZ_ALWAYS_INLINE constexpr auto has_zero(const uint64_t chunk) noexcept
   {
      return (((chunk - 0x0101010101010101) & ~chunk) & 0x8080808080808080);
   }

   GLZ_ALWAYS_INLINE constexpr auto has_quote(const uint64_t chunk) noexcept
   {
      return has_zero(chunk ^ repeat_byte('"'));
   }

   GLZ_ALWAYS_INLINE constexpr auto has_escape(const uint64_t chunk) noexcept
   {
      return has_zero(chunk ^ repeat_byte('\\'));
   }

   GLZ_ALWAYS_INLINE constexpr auto has_space(const uint64_t chunk) noexcept
   {
      return has_zero(chunk ^ repeat_byte(' '));
   }

   GLZ_ALWAYS_INLINE constexpr auto has_forward_slash(const uint64_t chunk) noexcept
   {
      return has_zero(chunk ^ repeat_byte('/'));
   }

   GLZ_ALWAYS_INLINE constexpr uint64_t is_less_16(const uint64_t c) noexcept
   {
      return has_zero(c & 0b1111000011110000111100001111000011110000111100001111000011110000);
   }

   GLZ_ALWAYS_INLINE constexpr uint64_t is_greater_15(const uint64_t c) noexcept
   {
      return (c & 0b1111000011110000111100001111000011110000111100001111000011110000);
   }

   template <opts Opts>
   GLZ_ALWAYS_INLINE void skip_ws_no_pre_check(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      while (true) {
         switch (*it) {
         case '\t':
         case '\n':
         case '\r':
         case ' ':
            ++it;
            break;
         case '/': {
            if constexpr (Opts.force_conformance) {
               ctx.error = error_code::syntax_error;
               return;
            }
            else {
               skip_comment(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               break;
            }
         }
         default:
            return;
         }
      }
   }

   template <opts Opts>
   GLZ_ALWAYS_INLINE void skip_ws(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      if (ctx.error == error_code::none) [[likely]] {
         skip_ws_no_pre_check<Opts>(ctx, it, end);
      }
   }

   GLZ_ALWAYS_INLINE void skip_till_escape_or_quote(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      static_assert(std::contiguous_iterator<std::decay_t<decltype(it)>>);

      const auto end_m7 = end - 7;
      for (; it < end_m7; it += 8) {
         uint64_t chunk;
         std::memcpy(&chunk, it, 8);
         const uint64_t test_chars = has_quote(chunk) | has_escape(chunk);
         if (test_chars) {
            it += (std::countr_zero(test_chars) >> 3);
            return;
         }
      }

      // Tail end of buffer. Should be rare we even get here
      while (it < end) {
         switch (*it) {
         case '\\':
         case '"':
            return;
         }
         ++it;
      }

      ctx.error = error_code::expected_quote;
   }

   GLZ_ALWAYS_INLINE void skip_till_quote(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      static_assert(std::contiguous_iterator<std::decay_t<decltype(it)>>);

      auto* pc = std::memchr(it, '"', std::distance(it, end));
      if (pc) [[likely]] {
         it = reinterpret_cast<std::decay_t<decltype(it)>>(pc);
         return;
      }

      ctx.error = error_code::expected_quote;
   }
   
   GLZ_ALWAYS_INLINE void skip_till_unescaped_quote(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      static_assert(std::contiguous_iterator<std::decay_t<decltype(it)>>);
      
      for (const auto fin = end - 31; it < fin;) {
         std::array<uint64_t, 4> chunk;
         std::memcpy(chunk.data(), it, 32);
         for (size_t i = 0; i < 4; ++i) {
            uint64_t test_chars = has_escape(chunk[i]) | has_quote(chunk[i]);
            if (test_chars) {
               it += (std::countr_zero(test_chars) >> 3);
               
               if (*it == '\\') {
                  it += 2;
               }
               else {
                  return;
               }
               break;
            }
            else {
               it += 8;
            }
         }
      }

      // Tail end of buffer. Should be rare we even get here
      while (it < end) {
         switch (*it) {
            case '\\': {
               ++it;
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::expected_quote;
                  return;
               }
               ++it;
               break;
            }
            case '"': {
               return;
            }
            default: {
               ++it;
            }
         }
      }
      
      ctx.error = error_code::expected_quote;
   }

   // very similar code to skip_till_quote, but it consumes the iterator and returns the key
   [[nodiscard]] GLZ_ALWAYS_INLINE const sv parse_unescaped_key(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      static_assert(std::contiguous_iterator<std::decay_t<decltype(it)>>);

      auto start = it;

      const auto end_m7 = end - 7;
      for (; it < end_m7; it += 8) {
         uint64_t chunk;
         std::memcpy(&chunk, it, 8);
         uint64_t test_chars = has_quote(chunk);
         if (test_chars) {
            it += (std::countr_zero(test_chars) >> 3);

            sv ret{start, size_t(it - start)};
            ++it;
            return ret;
         }
      }

      // Tail end of buffer. Should be rare we even get here
      while (it < end) {
         if (*it == '"') {
            sv ret{start, size_t(it - start)};
            ++it;
            return ret;
         }
         ++it;
      }
      ctx.error = error_code::expected_quote;
      return {};
   }

   // very similar code to skip_till_quote, but it consumes the iterator and returns the key
   template <uint32_t MinLength, uint32_t LengthRange>
   [[nodiscard]] GLZ_ALWAYS_INLINE const sv parse_key_cx(auto&& it) noexcept
   {
      static_assert(std::contiguous_iterator<std::decay_t<decltype(it)>>);

      auto start = it;
      it += MinLength; // immediately skip minimum length

      static_assert(LengthRange < 16);
      if constexpr (LengthRange == 7) {
         uint64_t chunk; // no need to default initialize
         std::memcpy(&chunk, it, 8);
         const uint64_t test_chunk = has_quote(chunk);
         if (test_chunk != 0) [[likely]] {
            it += (std::countr_zero(test_chunk) >> 3);

            return {start, size_t(it - start)};
         }
      }
      else if constexpr (LengthRange > 7) {
         uint64_t chunk; // no need to default initialize
         std::memcpy(&chunk, it, 8);
         uint64_t test_chunk = has_quote(chunk);
         if (test_chunk != 0) {
            it += (std::countr_zero(test_chunk) >> 3);

            return {start, size_t(it - start)};
         }
         else {
            it += 8;
            static constexpr auto rest = LengthRange + 1 - 8;
            chunk = 0; // must zero out the chunk
            std::memcpy(&chunk, it, rest);
            test_chunk = has_quote(chunk);
            if (test_chunk != 0) {
               it += (std::countr_zero(test_chunk) >> 3);

               return {start, size_t(it - start)};
            }
         }
      }
      else {
         uint64_t chunk{};
         std::memcpy(&chunk, it, LengthRange + 1);
         const uint64_t test_chunk = has_quote(chunk);
         if (test_chunk != 0) [[likely]] {
            it += (std::countr_zero(test_chunk) >> 3);

            return {start, size_t(it - start)};
         }
      }

      return {start, size_t(it - start)};
   }

   template <opts Opts>
   GLZ_ALWAYS_INLINE void skip_string(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      if (bool(ctx.error)) [[unlikely]] {
         return;
      }

      ++it;

      if constexpr (Opts.force_conformance) {
         while (true) {
            switch (*it) {
            case '"':
               ++it;
               return;
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t': {
               ctx.error = error_code::syntax_error;
               return;
            }
            case '\\': {
               ++it;
               switch (*it) {
               case '"':
               case '\\':
               case '/':
               case 'b':
               case 'f':
               case 'n':
               case 'r':
               case 't': {
                  ++it;
                  continue;
               }
               case 'u': {
                  ++it;
                  if ((end - it) < 4) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  else if (std::all_of(it, it + 4, ::isxdigit)) [[likely]] {
                     it += 4;
                  }
                  else [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  continue;
               }
                  [[unlikely]] default:
                  {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
            }
               [[likely]] default : ++it;
            }
         }
      }
      else {
         while (it < end) {
            if (*it == '"') {
               ++it;
               break;
            }
            else if (*it == '\\' && ++it == end) [[unlikely]]
               break;
            ++it;
         }
      }
   }

   template <char open, char close>
   GLZ_ALWAYS_INLINE void skip_until_closed(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      if (bool(ctx.error)) [[unlikely]] {
         return;
      }

      ++it;
      size_t open_count = 1;
      size_t close_count = 0;
      while (true) {
         switch (*it) {
         case '\0':
            ctx.error = error_code::unexpected_end;
            return;
         case '/':
            skip_comment(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            break;
         case '"':
            skip_string<opts{}>(ctx, it, end);
            break;
         case open:
            ++open_count;
            ++it;
            break;
         case close:
            ++close_count;
            ++it;
            if (close_count >= open_count) {
               return;
            }
            break;
         default:
            ++it;
         }
      }
   }

   GLZ_ALWAYS_INLINE constexpr bool is_numeric(const auto c) noexcept
   {
      switch (c) {
      case '0':
      case '1':
      case '2':
      case '3': //
      case '4':
      case '5':
      case '6':
      case '7': //
      case '8':
      case '9': //
      case '.':
      case '+':
      case '-': //
      case 'e':
      case 'E': //
         return true;
      }
      return false;
   }

   inline constexpr std::optional<uint64_t> stoui(const std::string_view s) noexcept
   {
      if (s.empty()) {
         return {};
      }

      uint64_t ret;
      auto* c = s.data();
      bool valid = detail::stoui64(ret, c);
      if (valid) {
         return ret;
      }
      return {};
   }

   GLZ_ALWAYS_INLINE void skip_number_with_validation(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      it += *it == '-';
      const auto sig_start_it = it;
      auto frac_start_it = end;
      if (*it == '0') {
         ++it;
         if (*it != '.') {
            return;
         }
         ++it;
         goto frac_start;
      }
      it = std::find_if_not(it, end, is_digit);
      if (it == sig_start_it) {
         ctx.error = error_code::syntax_error;
         return;
      }
      if ((*it | ('E' ^ 'e')) == 'e') {
         ++it;
         goto exp_start;
      }
      if (*it != '.') return;
      ++it;
   frac_start:
      frac_start_it = it;
      it = std::find_if_not(it, end, is_digit);
      if (it == frac_start_it) {
         ctx.error = error_code::syntax_error;
         return;
      }
      if ((*it | ('E' ^ 'e')) != 'e') return;
      ++it;
   exp_start:
      it += *it == '+' || *it == '-';
      const auto exp_start_it = it;
      it = std::find_if_not(it, end, is_digit);
      if (it == exp_start_it) {
         ctx.error = error_code::syntax_error;
         return;
      }
   }

   template <opts Opts>
   GLZ_ALWAYS_INLINE void skip_number(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      if constexpr (!Opts.force_conformance) {
         it = std::find_if_not(it + 1, end, is_numeric<char>);
      }
      else {
         skip_number_with_validation(ctx, it, end);
      }
   }

   template <opts Opts>
   GLZ_ALWAYS_INLINE void skip_value(is_context auto&& ctx, auto&& it, auto&& end) noexcept;

   // expects opening whitespace to be handled
   GLZ_ALWAYS_INLINE sv parse_key(is_context auto&& ctx, auto&& it, auto&& end) noexcept
   {
      // TODO this assumes no escapes.
      if (bool(ctx.error)) [[unlikely]]
         return {};

      match<'"'>(ctx, it, end);
      if (bool(ctx.error)) [[unlikely]]
         return {};
      auto start = it;
      skip_till_quote(ctx, it, end);
      if (bool(ctx.error)) [[unlikely]]
         return {};
      return sv{start, static_cast<size_t>(it++ - start)};
   }
   
   // clang-format off
   constexpr uint32_t digit_to_u32[]{ 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
         0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u, 0x00000005u, 0x00000006u, 0x00000007u, 0x00000008u, 0x00000009u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000000au, 0x0000000bu, 0x0000000cu, 0x0000000du, 0x0000000eu, 0x0000000fu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000000au, 0x0000000bu, 0x0000000cu, 0x0000000du,
         0x0000000eu, 0x0000000fu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0x00000000u, 0x00000010u, 0x00000020u, 0x00000030u, 0x00000040u, 0x00000050u, 0x00000060u, 0x00000070u, 0x00000080u, 0x00000090u, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x000000a0u, 0x000000b0u, 0x000000c0u, 0x000000d0u, 0x000000e0u, 0x000000f0u, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x000000a0u, 0x000000b0u,
         0x000000c0u, 0x000000d0u, 0x000000e0u, 0x000000f0u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000100u, 0x00000200u, 0x00000300u, 0x00000400u, 0x00000500u, 0x00000600u, 0x00000700u, 0x00000800u, 0x00000900u,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000a00u, 0x00000b00u, 0x00000c00u, 0x00000d00u, 0x00000e00u, 0x00000f00u,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0x00000a00u, 0x00000b00u, 0x00000c00u, 0x00000d00u, 0x00000e00u, 0x00000f00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00001000u, 0x00002000u, 0x00003000u, 0x00004000u, 0x00005000u, 0x00006000u, 0x00007000u,
         0x00008000u, 0x00009000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000a000u, 0x0000b000u, 0xc0000000u, 0x0000d000u,
         0x0000e000u, 0x0000f000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000a000u, 0x0000b000u, 0x0000c000u, 0x0000d000u, 0x0000e000u, 0x0000f000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
   // clang-format on
   
   GLZ_ALWAYS_INLINE uint32_t hex_to_u32_no_check(const auto* string1) noexcept {
      uint32_t v1 = digit_to_u32[630 + string1[0]];
      uint32_t v2 = digit_to_u32[420 + string1[1]];
      uint32_t v3 = digit_to_u32[210 + string1[2]];
      uint32_t v4 = digit_to_u32[0 + string1[3]];
      return v1 | v2 | v3 | v4;
   }
   
#if defined __has_builtin
#  if __has_builtin (__builtin_ia32_pdep_si)
#    #define pdep(x, y) __builtin_ia32_pdep_si(x, y)
#  endif
#endif
#ifndef pdep
   GLZ_ALWAYS_INLINE constexpr uint32_t pdep(uint32_t src, uint32_t mask) {
      uint32_t result  = 0;
      uint32_t src_bit = 1;

      for (uint32_t x = 0; x < 64; ++x) {
         if (mask & 1) {
            result |= (src & src_bit);
            src_bit <<= 1;
         }
         mask >>= 1;
      }

      return result;
   }
#endif
   
   template <class T>
   GLZ_ALWAYS_INLINE uint32_t code_point_to_utf8(uint32_t code_point, T* c) noexcept {
      if (code_point <= 0x7F) {
         c[0] = static_cast<T>(code_point);
         return 1;
      }
      const uint32_t leading_zeros = std::countl_zero(code_point);

      if (leading_zeros >= 11) {
         uint32_t pattern = pdep(0x3F00U, code_point);
         pattern |= 0xC0ull;
         c[0] = static_cast<T>(pattern >> 8);
         c[1] = static_cast<T>(pattern & 0xFFu);
         return 2;
      } else if (leading_zeros >= 16) {
         uint32_t pattern = pdep(0x0F0800U, code_point);
         pattern |= 0xE0ull;
         c[0] = static_cast<T>(pattern >> 16);
         c[1] = static_cast<T>(pattern >> 8);
         c[2] = static_cast<T>(pattern & 0xFFu);
         return 3;
      } else if (leading_zeros >= 21) {
         uint32_t pattern = pdep(0x01020400U, code_point);
         pattern |= 0xF0ull;
         c[0] = static_cast<T>(pattern >> 24);
         c[1] = static_cast<T>(pattern >> 16);
         c[2] = static_cast<T>(pattern >> 8);
         c[3] = static_cast<T>(pattern & 0xFFu);
         return 4;
      }
      return 0;
   }

   template <class T0, class T1>
   GLZ_ALWAYS_INLINE bool handle_unicode(T0* src, T1* dst) {
      static constexpr uint32_t sub_code_point = 0xfffd;
      uint32_t code_point = hex_to_u32_no_check(*src + 2);
      *src += 6;
      if (code_point >= 0xd800 && code_point < 0xdc00) {
         auto srcData = *src;
         if (((srcData[0] << 8) | srcData[1]) != ((static_cast<std::remove_pointer_t<T0>>(0x5Cu) << 8) | static_cast<std::remove_pointer_t<T0>>(0x75u))) {
            code_point = sub_code_point;
         } else {
            uint32_t code_point_2 = hex_to_u32_no_check(srcData + 2);
            uint32_t lowBit = code_point_2 - 0xdc00;
            if (lowBit >> 10) {
               code_point = sub_code_point;
            } else {
               code_point = (((code_point - 0xd800) << 10) | lowBit) + 0x10000;
               *src += 6;
            }
         }
      } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
         code_point = sub_code_point;
      }
      const auto offset = code_point_to_utf8(code_point, *dst);
      *dst += offset;
      return offset > 0;
   }
   
   /* Copyright (c) 2022 Tero 'stedo' Liukko, MIT License */
   GLZ_ALWAYS_INLINE unsigned char hex2dec(char hex) { return ((hex & 0xf) + (hex >> 6) * 9); }

   GLZ_ALWAYS_INLINE char32_t hex4_to_char32(const char* hex)
   {
      uint32_t value = hex2dec(hex[3]);
      value |= hex2dec(hex[2]) << 4;
      value |= hex2dec(hex[1]) << 8;
      value |= hex2dec(hex[0]) << 12;
      return value;
   }

   GLZ_ALWAYS_INLINE bool handle_escaped_unicode(auto*& src, auto*& dst)
   {
      src += 2;
      // This is slow but who is escaping unicode nowadays
      // codecvt is problematic on mingw hence mixing with the c character conversion functions
      if (!std::all_of(src, src + 4, ::isxdigit)) [[unlikely]] {
         return false;
      }
      
      char32_t codepoint = hex4_to_char32(src);
      
      src += 4;
      
      char8_t buffer[4];
      auto& facet = std::use_facet<std::codecvt<char32_t, char8_t, mbstate_t>>(std::locale());
      std::mbstate_t mbstate{};
      const char32_t* from_next;
      char8_t* to_next;
      const auto result =
         facet.out(mbstate, &codepoint, &codepoint + 1, from_next, buffer, buffer + 4, to_next);
      if (result != std::codecvt_base::ok) {
         return false;
      }
      
      const auto offset = size_t(to_next - buffer);
      std::memcpy(dst, buffer, offset);
      dst += offset;
      return true;
   }
   
   // clang-format off
   constexpr std::array<char, 256> char_unescape_table = { //
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
      0, 0, 0, 0, '"', 0, 0, 0, 0, 0, //
      0, 0, 0, 0, 0, 0, 0, '/', 0, 0, //
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
      0, 0, '\\', 0, 0, 0, 0, 0, '\b', 0, //
      0, 0, '\f', 0, 0, 0, 0, 0, 0, 0, //
      '\n', 0, 0, 0, '\r', 0, '\t', 0, 0, 0, //
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0 //
   };
   // clang-format on
   
   GLZ_ALWAYS_INLINE auto copy_and_find_delimiters(auto* string1, auto* string2, auto& swar) noexcept {
      using S = std::decay_t<decltype(swar)>;
      std::memcpy(&swar, string1, sizeof(S));
      std::memcpy(string2, string1, sizeof(S));
      return std::countr_zero(has_quote(swar) | has_escape(swar)) >> 3;
   }
   
   template <size_t N> requires (N == 8)
   GLZ_ALWAYS_INLINE char* parse_string(const auto* string1, auto* string2, uint64_t length_new) noexcept {
      uint64_t swar;
      while (length_new > 0) {
         uint64_t ix = copy_and_find_delimiters(string1, string2, swar);
         if (ix != 8) {
            auto escape_char = string1[ix];
            if (escape_char == '"') {
               return string2 + ix;
            } else if (escape_char == '\\') {
               escape_char = string1[ix + 1];
               if (escape_char == 'u') {
                  length_new -= ix;
                  string1 += ix;
                  string2 += ix;
                  if (!handle_escaped_unicode(string1, string2)) {
                     return {};
                  }
                  continue;
               }
               escape_char = char_unescape_table[escape_char];
               if (escape_char == 0) {
                  return {};
               }
               string2[ix] = escape_char;
               length_new -= ix + 2;
               string2 += ix + 1;
               string1 += ix + 2;
            }
         } else {
            length_new -= N;
            string2 += N;
            string1 += N;
         }
      }
      return string2;
   }
   
   template <size_t N> requires (N == 1)
   GLZ_ALWAYS_INLINE char* parse_string(const auto* string1, auto* string2, uint64_t length_new) noexcept {
      while (length_new > 0) {
         *string2 = *string1;
         if (*string1 == '"' || *string1 == '\\') {
            auto escape_char = *string1;
            if (escape_char == '"') {
               return string2;
            } else if (escape_char == '\\') {
               escape_char = string1[1];
               if (escape_char == 'u') {
                  if (!handle_escaped_unicode(string1, string2)) {
                     return {};
                  }
                  continue;
               }
               escape_char = char_unescape_table[escape_char];
               if (escape_char == 0) {
                  return {};
               }
               string2[0] = escape_char;
               length_new -= 2;
               string2 += 1;
               string1 += 2;
            }
         } else {
            --length_new;
            ++string2;
            ++string1;
         }
      }
      return string2;
   }
   
   template <auto multiple>
   GLZ_ALWAYS_INLINE auto round_up_to_multiple(const auto val) noexcept {
      const auto remainder = val % multiple;
      return remainder == 0 ? val : val + (multiple - remainder);
   }
}
