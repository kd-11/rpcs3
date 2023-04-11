#include "stdafx.h"
#include "RegisterAllocatorPass.h"

#include "Utilities/StrUtil.h"
#include <set>
#pragma optimize("", off)

namespace spv
{
	struct decoded_instruction_t
	{
		u32 address = umax;
		std::string output_reg;
		std::vector<std::string> input_regs;
		std::string lhs;
		std::string rhs;
	};

	struct register_usage_info_t
	{
		u32 first_read = umax;
		u32 first_write = umax;
		u32 last_read = 0;
		u32 last_write = 0;

		bool was_read_from() const
		{
			return first_read != umax;
		}

		bool was_written_to() const
		{
			return first_write != umax;
		}

		bool is_last_write(u32 address) const
		{
			return address == last_write;
		}

		bool requires_load() const
		{
			return !was_written_to() || first_read <= first_write;
		}

		bool requires_sync() const
		{
			return was_written_to() && was_read_from() && last_write < last_read;
		}

		bool is_const() const
		{
			return !was_written_to();
		}
	};

	std::pair<std::string, std::size_t> parse_reg(const std::string& token, std::size_t offset)
	{
		const auto start = token.find("vgpr[", offset);
		if (start == std::string::npos)
		{
			return { "", 0 };
		}

		const auto end = token.find_first_of("]", start);
		const auto reg_name = token.substr(start, end - start + 1);
		return { reg_name, end };
	}

	std::string get_temp_reg_name(const std::string& original_name)
	{
		std::string result;
		for (const auto& c : original_name)
		{
			if (c == ']' || c == '[')
			{
				continue;
			}

			result += c;
		}
		return result;
	}

	decoded_instruction_t decode_instruction(const spv::instruction_t& inst)
	{
		decoded_instruction_t result{};
		const auto lhs_end = inst.expression.find_first_of('=', 0);
		if (lhs_end == std::string::npos)
		{
			return result;
		}

		const auto lhs = inst.expression.substr(0, lhs_end);
		const auto rhs = inst.expression.substr(lhs_end + 1);

		result.output_reg = parse_reg(lhs, 0).first;

		std::size_t src_offset = 0;
		while (true)
		{
			const auto [reg_name, next_offset] = parse_reg(rhs, src_offset);
			if (reg_name.empty())
			{
				break;
			}
			src_offset = next_offset + 1;
			result.input_regs.push_back(reg_name);
		}

		result.address = inst.label;
		result.lhs = fmt::trim(lhs);
		result.rhs = fmt::trim(rhs);
		return result;
	}

	register_allocator_output run_allocator_pass(spv::function_t& function)
	{
		// Pass -1. Assign address labels.
		u32 last_label = umax;
		for (auto& inst : function.instructions)
		{
			if (inst.label != umax)
			{
				last_label = inst.label;
				continue;
			}

			if (inst.label == umax)
			{
				inst.label = last_label;
			}
		}

		// Pass 0. Decode all instructions.
		std::vector<decoded_instruction_t> decoded_instructions;
		for (std::size_t idx = 0; idx < function.instructions.size(); ++idx)
		{
			const auto& inst = function.instructions[idx];
			auto decoded = decode_instruction(inst);
			decoded.address = idx;
			decoded_instructions.push_back(decoded);
		}

		// Pass 1. List all registers referenced in this block.
		std::unordered_map<std::string, spv::register_usage_info_t> ref_map;

		// Pass 2. Map the register access values.
		for (const auto& inst : decoded_instructions)
		{
			if (!inst.output_reg.empty())
			{
				auto& output_ref = ref_map[inst.output_reg];
				output_ref.first_write = std::min(output_ref.first_write, inst.address);
				output_ref.last_write = inst.address;
			}

			for (const auto& input : inst.input_regs)
			{
				auto& input_ref = ref_map[input];
				input_ref.first_read = std::min(input_ref.first_read, inst.address);
				input_ref.last_read = inst.address;
			}
		}

		// Pass 4. Compile results.
		register_allocator_output result{};
		for (auto it = ref_map.begin(); it != ref_map.end(); ++it)
		{
			const auto& name = it->first;
			const auto& ref = it->second;

			if (ref.was_written_to() && !ref.was_read_from())
			{
				continue;
			}

			if (!ref.was_written_to() && ref.first_read == ref.last_read)
			{
				// Single load
				continue;
			}

			register_initializer_t reg_init;
			reg_init.require_load = ref.requires_load();
			reg_init.require_sync = ref.requires_sync();
			reg_init.is_const = ref.is_const();
			reg_init.old_name = name;
			reg_init.new_name = get_temp_reg_name(name);
			result.temp_regs[name] = reg_init;
		}

		// Pass 5. Actual renaming
		std::vector<std::pair<std::string, std::string>> replacements;
		std::set<std::string> reg_set;

		auto do_replacements = [&result, &replacements](const std::string& expr, const std::set<std::string>& regs)
		{
			replacements.clear();

			for (const auto& reg : regs)
			{
				if (result.temp_regs.find(reg) == result.temp_regs.end())
				{
					continue;
				}

				const auto& info = result.temp_regs[reg];
				if (info.new_name == info.old_name || info.new_name.empty())
				{
					continue;
				}

				replacements.push_back({ info.old_name, info.new_name });
			}

			return fmt::replace_all(expr, replacements);
		};

		int loop_depth = 0;
		int last_exit = -1;

		for (std::size_t idx = 0; idx < function.instructions.size(); ++idx)
		{
			auto& inst = function.instructions[idx];
			auto& decoded = decoded_instructions[idx];

			if (inst.expression.empty() || decoded.address == umax)
			{
				continue;
			}

			if (inst.flags.loop_enter)
			{
				loop_depth++;
			}
			else if (inst.flags.loop_exit)
			{
				loop_depth--;
			}

			if (inst.expression == "return;") // TODO: Flags
			{
				last_exit = idx;
			}

			reg_set.clear();
			for (const auto& reg : decoded.input_regs)
			{
				reg_set.insert(reg);
			}

			const auto rhs = do_replacements(decoded.rhs, reg_set);
			auto lhs = decoded.lhs;

			if (!decoded.output_reg.empty())
			{
				if (auto reg_info = ref_map.find(decoded.output_reg); reg_info != ref_map.end())
				{
					// Register can be replaced.
					bool to_replace = (reg_info->second.requires_sync() || !reg_info->second.is_last_write(idx));
					if (!to_replace && reg_info->second.is_last_write(idx) && loop_depth > 0)
					{
						// Last write, but we're in a loop. Force exit sync.
						auto& reg_init = result.temp_regs[decoded.output_reg];
						reg_init.require_sync = true;
						if (reg_init.new_name.empty())
						{
							reg_init.require_load = false;
							reg_init.old_name = decoded.output_reg;
							reg_init.new_name = get_temp_reg_name(decoded.output_reg);
						}

						to_replace = true;
					}

					if (to_replace)
					{
						lhs = do_replacements(lhs, { decoded.output_reg });

						if (reg_info->second.requires_sync() &&
							last_exit < reg_info->second.first_write &&
							loop_depth == 0 &&
							reg_info->second.is_last_write(idx) &&
							decoded.lhs == decoded.output_reg &&
							!rhs.starts_with('='))
						{
							result.temp_regs[decoded.output_reg].require_sync = false; // No need for exit sync
							lhs = decoded.output_reg + " = " + lhs;
						}
					}
				}
			}

			if (lhs == decoded.lhs && rhs == decoded.rhs)
			{
				continue;
			}

			// Take care, special expressions like == >= <= exist
			const std::string concat = rhs.starts_with('=') ? " = " : " = ";
			inst.expression = lhs + concat + rhs;
		}

		return result;
	}
}
