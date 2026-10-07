// Small JSON value for mods (web functions, save data). Self-contained: no other library needed.
//   ml::Json reply = {{"ok", true}, {"cash", 1500}};
//   reply["items"] = ml::Json::Array({1, 2, 3});
//   ml::Json args = ml::Json::Parse(text);  int item = args[0].Int();
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ml
{
	class Json
	{
	public:
		enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
		using ArrayT = std::vector<Json>;
		using ObjectT = std::map<std::string, Json>;

		Json() = default;
		Json(std::nullptr_t) {}
		Json(bool b) : m_type(Type::Bool), m_bool(b) {}
		template<class T>
		    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
		Json(T v) : m_type(Type::Number), m_int(static_cast<int64_t>(v)), m_num(static_cast<double>(v)), m_isInt(true)
		{
		}
		template<class T>
		    requires std::is_floating_point_v<T>
		Json(T v) : m_type(Type::Number), m_num(static_cast<double>(v))
		{
		}
		Json(const char* s) : m_type(Type::String), m_str(s ? s : "") {}
		Json(std::string s) : m_type(Type::String), m_str(std::move(s)) {}
		Json(std::string_view s) : m_type(Type::String), m_str(s) {}
		// Object: {{"key", value}, ...}
		Json(std::initializer_list<std::pair<const std::string, Json>> members) : m_type(Type::Object), m_obj(std::make_shared<ObjectT>(members)) {}
		template<class T>
		Json(const std::vector<T>& items) : m_type(Type::Array), m_arr(std::make_shared<ArrayT>(items.begin(), items.end()))
		{
		}
		template<class T>
		Json(const std::map<std::string, T>& members) : m_type(Type::Object), m_obj(std::make_shared<ObjectT>(members.begin(), members.end()))
		{
		}

		static Json Array(std::initializer_list<Json> items = {})
		{
			Json j;
			j.m_type = Type::Array;
			j.m_arr = std::make_shared<ArrayT>(items);
			return j;
		}
		static Json Object()
		{
			Json j;
			j.m_type = Type::Object;
			j.m_obj = std::make_shared<ObjectT>();
			return j;
		}

		// Copies are deep (values, not references).
		Json(const Json& o) { *this = o; }
		Json& operator=(const Json& o)
		{
			if (this == &o)
				return *this;
			m_type = o.m_type;
			m_bool = o.m_bool;
			m_int = o.m_int;
			m_num = o.m_num;
			m_isInt = o.m_isInt;
			m_str = o.m_str;
			m_arr = o.m_arr ? std::make_shared<ArrayT>(*o.m_arr) : nullptr;
			m_obj = o.m_obj ? std::make_shared<ObjectT>(*o.m_obj) : nullptr;
			return *this;
		}
		Json(Json&&) noexcept = default;
		Json& operator=(Json&&) noexcept = default;

		Type GetType() const { return m_type; }
		bool IsNull() const { return m_type == Type::Null; }
		bool IsBool() const { return m_type == Type::Bool; }
		bool IsNumber() const { return m_type == Type::Number; }
		bool IsString() const { return m_type == Type::String; }
		bool IsArray() const { return m_type == Type::Array; }
		bool IsObject() const { return m_type == Type::Object; }

		// Values, with a fallback when the type does not match.
		bool Bool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : m_type == Type::Number ? m_num != 0 : fallback; }
		int64_t Int64(int64_t fallback = 0) const
		{
			return m_type != Type::Number ? fallback : m_isInt ? m_int : static_cast<int64_t>(m_num);
		}
		int32_t Int(int32_t fallback = 0) const { return static_cast<int32_t>(Int64(fallback)); }
		double Number(double fallback = 0) const { return m_type == Type::Number ? m_num : fallback; }
		float Float(float fallback = 0) const { return static_cast<float>(Number(fallback)); }
		const std::string& Str() const
		{
			static const std::string empty;
			return m_type == Type::String ? m_str : empty;
		}
		std::string Str(std::string_view fallback) const { return m_type == Type::String ? m_str : std::string(fallback); }

		// Converts to T (bool, integers, floating point, std::string, Json, std::vector<...>).
		template<class T>
		T Get() const
		{
			if constexpr (std::is_same_v<T, Json>)
				return *this;
			else if constexpr (std::is_same_v<T, bool>)
				return Bool();
			else if constexpr (std::is_integral_v<T>)
				return static_cast<T>(Int64());
			else if constexpr (std::is_floating_point_v<T>)
				return static_cast<T>(Number());
			else if constexpr (std::is_same_v<T, std::string>)
				return Str();
			else
			{
				T out{};
				for (const Json& item : Items())
					out.push_back(item.template Get<typename T::value_type>());
				return out;
			}
		}

		// Arrays and objects.
		size_t Size() const { return m_arr ? m_arr->size() : m_obj ? m_obj->size() : 0; }
		bool Contains(const std::string& key) const { return m_obj && m_obj->contains(key); }
		// Missing members / elements read as null.
		const Json& operator[](const std::string& key) const
		{
			if (m_obj)
				if (const auto it = m_obj->find(key); it != m_obj->end())
					return it->second;
			return Null();
		}
		const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
		const Json& operator[](size_t i) const { return m_arr && i < m_arr->size() ? (*m_arr)[i] : Null(); }
		const Json& operator[](int i) const { return i < 0 ? Null() : (*this)[static_cast<size_t>(i)]; }
		// Writing a member turns null into an object.
		Json& operator[](const std::string& key)
		{
			if (m_type == Type::Null)
				*this = Object();
			if (!m_obj)
				return Scratch();
			return (*m_obj)[key];
		}
		Json& operator[](const char* key) { return (*this)[std::string(key)]; }
		// Appending turns null into an array.
		void Push(Json value)
		{
			if (m_type == Type::Null)
				*this = Array();
			if (m_arr)
				m_arr->push_back(std::move(value));
		}
		void Erase(const std::string& key)
		{
			if (m_obj)
				m_obj->erase(key);
		}
		const ArrayT& Items() const
		{
			static const ArrayT empty;
			return m_arr ? *m_arr : empty;
		}
		const ObjectT& Members() const
		{
			static const ObjectT empty;
			return m_obj ? *m_obj : empty;
		}

		std::string Dump() const
		{
			std::string out;
			DumpTo(out);
			return out;
		}

		// Null when `text` is not valid JSON (`ok` tells).
		static Json Parse(std::string_view text, bool* ok = nullptr)
		{
			size_t at = 0;
			Json value;
			bool good = ParseValue(text, at, value, 0);
			SkipSpace(text, at);
			good = good && at == text.size();
			if (ok)
				*ok = good;
			return good ? value : Json();
		}

		static std::string Quote(std::string_view s)
		{
			std::string out = "\"";
			for (const char c : s)
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20)
						out += std::format("\\u{:04x}", static_cast<int>(c));
					else
						out += c;
				}
			return out + "\"";
		}

	private:
		static const Json& Null()
		{
			static const Json null;
			return null;
		}
		static Json& Scratch()
		{
			static Json scratch;
			scratch = Json();
			return scratch;
		}

		void DumpTo(std::string& out) const
		{
			switch (m_type)
			{
			case Type::Null: out += "null"; break;
			case Type::Bool: out += m_bool ? "true" : "false"; break;
			case Type::Number:
				if (m_isInt)
					out += std::to_string(m_int);
				else if (!std::isfinite(m_num))
					out += "null";
				else
					out += std::format("{}", m_num);
				break;
			case Type::String: out += Quote(m_str); break;
			case Type::Array:
			{
				out += '[';
				bool first = true;
				for (const Json& item : *m_arr)
				{
					if (!first)
						out += ',';
					first = false;
					item.DumpTo(out);
				}
				out += ']';
				break;
			}
			case Type::Object:
			{
				out += '{';
				bool first = true;
				for (const auto& [key, value] : *m_obj)
				{
					if (!first)
						out += ',';
					first = false;
					out += Quote(key);
					out += ':';
					value.DumpTo(out);
				}
				out += '}';
				break;
			}
			}
		}

		static void SkipSpace(std::string_view t, size_t& at)
		{
			while (at < t.size() && (t[at] == ' ' || t[at] == '\n' || t[at] == '\r' || t[at] == '\t'))
				++at;
		}

		static void AppendUtf8(std::string& out, uint32_t cp)
		{
			if (cp < 0x80)
				out += static_cast<char>(cp);
			else if (cp < 0x800)
			{
				out += static_cast<char>(0xC0 | cp >> 6);
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else if (cp < 0x10000)
			{
				out += static_cast<char>(0xE0 | cp >> 12);
				out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | cp >> 18);
				out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
				out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
		}

		static bool ParseHex4(std::string_view t, size_t at, uint32_t& v)
		{
			if (at + 4 > t.size())
				return false;
			v = 0;
			for (size_t i = 0; i < 4; ++i)
			{
				const char c = t[at + i];
				v <<= 4;
				if (c >= '0' && c <= '9')
					v |= c - '0';
				else if (c >= 'a' && c <= 'f')
					v |= c - 'a' + 10;
				else if (c >= 'A' && c <= 'F')
					v |= c - 'A' + 10;
				else
					return false;
			}
			return true;
		}

		static bool ParseString(std::string_view t, size_t& at, std::string& out)
		{
			if (at >= t.size() || t[at] != '"')
				return false;
			++at;
			while (at < t.size())
			{
				const char c = t[at++];
				if (c == '"')
					return true;
				if (c != '\\')
				{
					out += c;
					continue;
				}
				if (at >= t.size())
					return false;
				switch (const char e = t[at++])
				{
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u':
				{
					uint32_t cp;
					if (!ParseHex4(t, at, cp))
						return false;
					at += 4;
					if (cp >= 0xD800 && cp < 0xDC00 && at + 6 <= t.size() && t[at] == '\\' && t[at + 1] == 'u')
						if (uint32_t low; ParseHex4(t, at + 2, low) && low >= 0xDC00 && low < 0xE000)
						{
							cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
							at += 6;
						}
					AppendUtf8(out, cp);
					break;
				}
				default: return false;
				}
			}
			return false;
		}

		static bool ParseValue(std::string_view t, size_t& at, Json& out, int depth)
		{
			if (depth > 200)
				return false;
			SkipSpace(t, at);
			if (at >= t.size())
				return false;
			const char c = t[at];
			if (c == '{')
			{
				++at;
				out = Object();
				SkipSpace(t, at);
				if (at < t.size() && t[at] == '}')
					return ++at, true;
				for (;;)
				{
					SkipSpace(t, at);
					std::string key;
					if (!ParseString(t, at, key))
						return false;
					SkipSpace(t, at);
					if (at >= t.size() || t[at++] != ':')
						return false;
					Json value;
					if (!ParseValue(t, at, value, depth + 1))
						return false;
					(*out.m_obj)[key] = std::move(value);
					SkipSpace(t, at);
					if (at < t.size() && t[at] == ',')
					{
						++at;
						continue;
					}
					return at < t.size() && t[at++] == '}';
				}
			}
			if (c == '[')
			{
				++at;
				out = Array();
				SkipSpace(t, at);
				if (at < t.size() && t[at] == ']')
					return ++at, true;
				for (;;)
				{
					Json value;
					if (!ParseValue(t, at, value, depth + 1))
						return false;
					out.m_arr->push_back(std::move(value));
					SkipSpace(t, at);
					if (at < t.size() && t[at] == ',')
					{
						++at;
						continue;
					}
					return at < t.size() && t[at++] == ']';
				}
			}
			if (c == '"')
			{
				std::string s;
				if (!ParseString(t, at, s))
					return false;
				out = Json(std::move(s));
				return true;
			}
			const auto word = [&](std::string_view w) {
				if (t.substr(at, w.size()) != w)
					return false;
				at += w.size();
				return true;
			};
			if (word("true"))
				return out = Json(true), true;
			if (word("false"))
				return out = Json(false), true;
			if (word("null"))
				return out = Json(), true;
			// Number
			const size_t start = at;
			if (at < t.size() && t[at] == '-')
				++at;
			bool isInt = true;
			while (at < t.size() && ((t[at] >= '0' && t[at] <= '9') || t[at] == '.' || t[at] == 'e' || t[at] == 'E' || t[at] == '+' || t[at] == '-'))
			{
				if (t[at] == '.' || t[at] == 'e' || t[at] == 'E')
					isInt = false;
				++at;
			}
			if (at == start)
				return false;
			const std::string number(t.substr(start, at - start));
			char* end = nullptr;
			if (isInt)
			{
				const long long v = std::strtoll(number.c_str(), &end, 10);
				if (end == number.c_str() + number.size())
					return out = Json(static_cast<int64_t>(v)), true;
			}
			const double d = std::strtod(number.c_str(), &end);
			if (end != number.c_str() + number.size())
				return false;
			out = Json(d);
			return true;
		}

		Type m_type = Type::Null;
		bool m_bool = false;
		int64_t m_int = 0;
		double m_num = 0;
		bool m_isInt = false;
		std::string m_str;
		std::shared_ptr<ArrayT> m_arr;
		std::shared_ptr<ObjectT> m_obj;
	};
}
