#include "ypt.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <string_view>

#include "block.hpp"
#include "yft.hpp"

namespace loader::convert
{
	namespace
	{
		// Learned from the game's legacy and Enhanced particle files (tools/learn_ptx.py): object type ids, and per
		// shader technique its index and variable list.
		struct PtxVar
		{
			uint32_t hash;
			uint32_t type;  // 2/4 float values, 6 texture, 7 keyframe
			uint32_t index;
			uint32_t flag;
			std::array<uint8_t, 0x20> values; // +0x20..+0x40 of float variables
		};
		struct PtxTechnique
		{
			std::string_view shader, technique;
			uint32_t index;
			std::vector<PtxVar> vars;
		};
#include "ypt_tables.inc"

		// Particle rule: name +0x120, behaviour lists +0x128/+0x138/+0x148 (atArray), shader +0x1B8, technique +0x1C0,
		// technique index +0x1D0, variables +0x1F0 (atArray).
		constexpr size_t kRuleBehaviours[] = {0x128, 0x138, 0x148};
		constexpr size_t kRuleShader = 0x1B8, kRuleTechnique = 0x1C0, kRuleTechniqueIndex = 0x1D0, kRuleVars = 0x1F0;
		// Behaviours: name hash at +8.
		constexpr uint32_t kGrown = 0xF5B33BAA;    // 0x30 -> 0x40 bytes (zeros added)
		constexpr uint64_t kVarFloat = 0x1406EDFE8, kVarKeyframe = 0x1406EE070, kVarTexture = 0x1406EE0F8;

		std::string Str(const Block& blk, uint64_t p)
		{
			std::string s;
			if (const auto o = blk.Ptr(p))
				for (size_t c = *o; c < blk.data.size() && blk.data[c] && s.size() < 256; ++c)
					s += static_cast<char>(blk.data[c]);
			return s;
		}

		const PtxTechnique* FindTechnique(const std::string& shader, const std::string& technique)
		{
			for (const auto& t : kTechniques)
				if (t.shader == shader && t.technique == technique)
					return &t;
			return nullptr;
		}

		struct Array
		{
			size_t field; // offset of the atArray
			std::optional<size_t> data;
			uint16_t count;
		};
		Array ArrayAt(const Block& blk, size_t field)
		{
			return {field, blk.Ptr(blk.U64(field)), blk.U16(field + 8)};
		}

		bool ConvertRule(Block& blk, size_t rule, std::optional<size_t>& emptyString, std::vector<std::string>& warnings, std::string& error)
		{
			const std::string name = Str(blk, blk.U64(rule + 0x120));
			const std::string shader = Str(blk, blk.U64(rule + kRuleShader)), technique = Str(blk, blk.U64(rule + kRuleTechnique));
			const PtxTechnique* t = FindTechnique(shader, technique);
			if (!t)
			{
				warnings.push_back(std::format("粒子規則 {}：shader {} / {} 在強化版沒有對應，沿用原本的設定", name, shader, technique));
				return true;
			}
			Put<uint32_t>(blk.data, rule + kRuleTechniqueIndex, t->index);

			// Legacy variables by name hash.
			std::map<uint32_t, size_t> legacy;
			const Array vars = ArrayAt(blk, rule + kRuleVars);
			for (uint16_t i = 0; vars.data && i < vars.count; ++i)
				if (const auto v = blk.Ptr(blk.U64(*vars.data + 8 * i)))
					legacy[blk.U32(*v + 0x10)] = *v;

			const auto list = blk.Alloc(8 * std::max<size_t>(1, t->vars.size()));
			if (!list)
			{
				error = "out of space";
				return false;
			}
			for (size_t i = 0; i < t->vars.size(); ++i)
			{
				const PtxVar& var = t->vars[i];
				const size_t size = var.type == 7 ? 0x50 : 0x40;
				const auto o = blk.Alloc(size);
				if (!o)
				{
					error = "out of space";
					return false;
				}
				Bytes& b = blk.data;
				const auto old = legacy.find(var.hash);
				if (old != legacy.end() && blk.U32(old->second + 0x14) == var.type)
				{
					std::copy(b.begin() + old->second + 0x20, b.begin() + old->second + size, b.begin() + *o + 0x20);
					if (var.type == 6)
						Put<uint32_t>(b, *o + 0x3C, 0); // legacy texture variables have 1 here, the game's files 0
				}
				else if (var.type == 6)
				{
					// A texture variable without a texture: an empty name.
					if (!emptyString && !(emptyString = blk.Alloc(16)))
					{
						error = "out of space";
						return false;
					}
					Put<uint64_t>(b, *o + 0x30, kVirtual + *emptyString);
				}
				else if (var.type != 7)
					std::copy(var.values.begin(), var.values.end(), b.begin() + *o + 0x20);
				Put<uint64_t>(b, *o, var.type == 7 ? kVarKeyframe : var.type == 6 ? kVarTexture : kVarFloat);
				Put<uint32_t>(b, *o + 0x10, var.hash);
				Put<uint32_t>(b, *o + 0x14, var.type);
				Put<uint32_t>(b, *o + 0x18, var.index);
				Put<uint32_t>(b, *o + 0x1C, var.flag);
				Put<uint64_t>(b, *list + 8 * i, kVirtual + *o);
			}
			Put<uint64_t>(blk.data, rule + kRuleVars, kVirtual + *list);
			Put<uint16_t>(blk.data, rule + kRuleVars + 8, static_cast<uint16_t>(t->vars.size()));
			Put<uint16_t>(blk.data, rule + kRuleVars + 10, static_cast<uint16_t>(t->vars.size()));
			return true;
		}

		// Behaviour lists: kGrown behaviours move into larger objects.
		bool ConvertBehaviours(Block& blk, const std::vector<size_t>& rules, std::string& error)
		{
			std::map<size_t, size_t> moved;
			for (const size_t rule : rules)
				for (const size_t field : kRuleBehaviours)
				{
					const Array a = ArrayAt(blk, rule + field);
					if (!a.data)
						continue;
					uint16_t kept = 0;
					for (uint16_t i = 0; i < a.count; ++i)
					{
						const uint64_t p = blk.U64(*a.data + 8 * i);
						const auto b = blk.Ptr(p);
						if (!b)
							continue;
						const uint32_t hash = blk.U32(*b + 8);
						uint64_t to = p;
						if (hash == kGrown)
						{
							auto it = moved.find(*b);
							if (it == moved.end())
							{
								const auto n = blk.Alloc(0x40);
								if (!n)
								{
									error = "out of space";
									return false;
								}
								std::copy(blk.data.begin() + *b, blk.data.begin() + *b + 0x30, blk.data.begin() + *n);
								it = moved.emplace(*b, *n).first;
							}
							to = kVirtual + it->second;
						}
						Put<uint64_t>(blk.data, *a.data + 8 * kept++, to);
					}
					for (uint16_t i = kept; i < a.count; ++i)
						Put<uint64_t>(blk.data, *a.data + 8 * i, 0);
					Put<uint16_t>(blk.data, rule + field + 8, kept);
				}
			return true;
		}
	}

	int g_particleDebugSkip = 0;

	bool ConvertParticleResource(const Bytes& legacy, const std::unordered_map<uint32_t, Effect>& effects, Bytes& out,
	    std::vector<std::string>& warnings, std::string& error)
	{
		Resource in;
		if (!Read(legacy, in, error))
			return false;
		if (in.version != kLegacyParticleVersion)
		{
			error = std::format("unexpected resource version {}", in.version);
			return false;
		}
		Block blk;
		blk.Init(in);

		// Object type ids (the first 8 bytes of every ptx object) -> the ids of the game's own files.
		size_t renamed = (g_particleDebugSkip & 1) ? 1 : 0;
		for (size_t o = 0; !(g_particleDebugSkip & 1) && o + 8 <= blk.data.size(); o += 8)
			for (const auto& [from, to] : kTypeIds)
				if (blk.U64(o) == from)
				{
					Put<uint64_t>(blk.data, o, to);
					++renamed;
					break;
				}
		if (!renamed)
		{
			error = "no known particle objects (unknown tool?)";
			return false;
		}

		// Root (ptxFxList): +0x20 textures, +0x30 models, +0x38 particle rules (pgDictionary: values +0x30, count +0x38).
		std::vector<size_t> rules;
		if (const auto dict = blk.Ptr(blk.U64(0x38)))
		{
			const Array values = ArrayAt(blk, *dict + 0x30);
			for (uint16_t i = 0; values.data && i < values.count; ++i)
				if (const auto r = blk.Ptr(blk.U64(*values.data + 8 * i)))
					rules.push_back(*r);
		}
		std::optional<size_t> emptyString;
		for (const size_t rule : rules)
			if (!(g_particleDebugSkip & 2) && !ConvertRule(blk, rule, emptyString, warnings, error))
				return false;
		if (!(g_particleDebugSkip & 4) && !ConvertBehaviours(blk, rules, error))
			return false;

		if (const auto txd = blk.Ptr(blk.U64(0x20)); txd && !ConvertEmbeddedTextures(blk, *txd, error))
			return false;
		return ConvertDrawables(blk, effects, warnings, error) && FinishResource(blk, kEnhancedParticleVersion, out, error);
	}
}
