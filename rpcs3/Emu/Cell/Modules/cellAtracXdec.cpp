#include "stdafx.h"
#include "Emu/perf_meter.hpp"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/lv2/sys_ppu_thread.h"
#include "Emu/savestate_utils.hpp"
#include "sysPrxForUser.h"
#include "util/asm.hpp"
#include "util/media_utils.h"

#include "cellAtracXdec.h"

vm::gvar<CellAdecCoreOps> g_cell_adec_core_ops_atracx2ch;
vm::gvar<CellAdecCoreOps> g_cell_adec_core_ops_atracx6ch;
vm::gvar<CellAdecCoreOps> g_cell_adec_core_ops_atracx8ch;
vm::gvar<CellAdecCoreOps> g_cell_adec_core_ops_atracx;

LOG_CHANNEL(cellAtracXdec);

template <>
void fmt_class_string<CellAtracXdecError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](CellAtracXdecError value)
	{
		switch (value)
		{
		STR_CASE(CELL_ADEC_ERROR_ATX_OK); // CELL_ADEC_ERROR_ATX_OFFSET, CELL_ADEC_ERROR_ATX_NONE
		STR_CASE(CELL_ADEC_ERROR_ATX_BUSY);
		STR_CASE(CELL_ADEC_ERROR_ATX_EMPTY);
		STR_CASE(CELL_ADEC_ERROR_ATX_ATSHDR);
		STR_CASE(CELL_ADEC_ERROR_ATX_NON_FATAL);
		STR_CASE(CELL_ADEC_ERROR_ATX_NOT_IMPLE);
		STR_CASE(CELL_ADEC_ERROR_ATX_PACK_CE_OVERFLOW);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILLEGAL_NPROCQUS);
		STR_CASE(CELL_ADEC_ERROR_ATX_FATAL);
		STR_CASE(CELL_ADEC_ERROR_ATX_ENC_OVERFLOW);
		STR_CASE(CELL_ADEC_ERROR_ATX_PACK_CE_UNDERFLOW);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDCT);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GAINADJ);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_SPECTRA);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GHWAVE);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_SHEADER);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_B);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_C);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_D);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDWL_E);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF_B);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF_C);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDSF_D);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_IDCT_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_NGC);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_IDLEV_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_IDLOC_A);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_IDLEV_B);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_GC_IDLOC_B);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_SN_NWVS);
		STR_CASE(CELL_ADEC_ERROR_ATX_FATAL_HANDLE);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_SAMPLING_FREQ);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_CH_CONFIG_INDEX);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_NBYTES);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_BLOCK_NUM);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_BLOCK_ID);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_CHANNELS);
		STR_CASE(CELL_ADEC_ERROR_ATX_UNINIT_BLOCK_SPECIFIED);
		STR_CASE(CELL_ADEC_ERROR_ATX_POSCFG_PRESENT);
		STR_CASE(CELL_ADEC_ERROR_ATX_BUFFER_OVERFLOW);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_BLK_TYPE_ID);
		STR_CASE(CELL_ADEC_ERROR_ATX_UNPACK_CHANNEL_BLK_FAILED);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_BLK_ID_USED_1);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_BLK_ID_USED_2);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILLEGAL_ENC_SETTING);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILLEGAL_DEC_SETTING);
		STR_CASE(CELL_ADEC_ERROR_ATX_ASSERT_NSAMPLES);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_SYNCWORD);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_SAMPLING_FREQ);
		STR_CASE(CELL_ADEC_ERROR_ATX_ILL_CH_CONFIG_INDEX);
		STR_CASE(CELL_ADEC_ERROR_ATX_RAW_DATA_FRAME_SIZE_OVER);
		STR_CASE(CELL_ADEC_ERROR_ATX_SYNTAX_ENHANCE_LENGTH_OVER);
		STR_CASE(CELL_ADEC_ERROR_ATX_SPU_INTERNAL_FAIL);
		}

		return unknown;
	});
}

namespace atracxdec_internal
{
	std::optional<ats_header_info> parse_ats_header(const AtracXdecAtsHeader& ats)
	{
		if (ats.sync_word != 0x0fd0)
		{
			return std::nullopt;
		}

		const u8 sample_rate_idx = ats.params >> 13;
		const u8 ch_config_idx = ats.params >> 10 & 7;
		const u16 nbytes = ((ats.params & 0x3ff) + 1) * 8;

		if (ch_config_idx == 0u)
		{
			return std::nullopt;
		}

		switch (sample_rate_idx)
		{
		case 1: return ats_header_info{ 44100, ch_config_idx, nbytes };
		case 2: return ats_header_info{ 48000, ch_config_idx, nbytes };
		default: return std::nullopt;
		}
	}

	// Returns the host object of an open decoder, verifying that it belongs to the given context
	static stx::shared_ptr<AtracXdecHost> get_host(vm::ptr<AtracXdecContext> handle)
	{
		if (!handle || !vm::check_addr(handle.addr(), vm::page_readable, sizeof(AtracXdecContext)))
		{
			return {};
		}

		auto host = idm::get_unlocked<AtracXdecHost>(handle->decoder.host_id);

		if (!host || host->owner_addr != handle.addr() || host->closed)
		{
			return {};
		}

		return host;
	}

	// Finds the host object of a context without using the ID stored in guest memory. Also returns closed objects
	static std::pair<u32, stx::shared_ptr<AtracXdecHost>> find_host(u32 owner_addr)
	{
		u32 host_id = 0;

		auto found = idm::select<AtracXdecHost>([&](u32 id, AtracXdecHost& host) -> bool
		{
			if (host.owner_addr != owner_addr)
			{
				return false;
			}

			host_id = id;
			return true;
		});

		return { host_id, std::move(found.ptr) };
	}
}


AtracXdecHost::AtracXdecHost(u32 owner_addr, u32 max_nch)
	: owner_addr(owner_addr), max_nch(max_nch), work_mem_addr(owner_addr + atracxdec_internal::WORK_MEM_OFFSET)
{
	alloc_avcodec();
}

AtracXdecHost::AtracXdecHost(utils::serial& ar)
	: owner_addr(ar.pop<u32>()), max_nch(ar.pop<u32>()), work_mem_addr(owner_addr + atracxdec_internal::WORK_MEM_OFFSET)
{
	worker_thread_id = ar.pop<u32>();

	const u32 _sampling_freq = ar.pop<u32>();
	const u32 _ch_config_idx = ar.pop<u32>();
	const u32 _nbytes = ar.pop<u32>();
	const u32 _bw_pcm = ar.pop<u32>();
	const u32 _nch_out = ar.pop<u32>();
	const bool _config_is_set = ar.pop<bool>();
	const bool _decoder_ready = ar.pop<bool>();

	alloc_avcodec();

	// The savestate is not trusted, the configuration is validated again and FFmpeg is reinitialized
	if (max_nch != 2u && max_nch != 6u && max_nch != 8u)
	{
		cellAtracXdec.error("AtracXdecHost: Invalid savestate data (max_nch=%d)", max_nch);
		closed = true;
		return;
	}

	if (_config_is_set && set_config_info(_sampling_freq, _ch_config_idx, _nbytes) == CELL_OK && _decoder_ready)
	{
		init_decode(_bw_pcm, _nch_out);
	}
}

AtracXdecHost::~AtracXdecHost()
{
	if (packet)
	{
		av_packet_free(&packet);
	}
	if (frame)
	{
		av_frame_free(&frame);
	}
	if (ctx)
	{
		avcodec_free_context(&ctx);
	}
}

void AtracXdecHost::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(cellAtracXdec);

	ar(owner_addr, max_nch, worker_thread_id.load(), sampling_freq, ch_config_idx, nbytes, bw_pcm, nch_out, config_is_set, decoder_ready);
}

void AtracXdecHost::alloc_avcodec()
{
	codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
	packet = av_packet_alloc();
	frame = av_frame_alloc();

	if (!is_valid())
	{
		cellAtracXdec.error("AtracXdecHost: Failed to allocate the FFmpeg decoder (codec=%d, packet=%d, frame=%d)", !!codec, !!packet, !!frame);
	}
}

bool AtracXdecHost::init_avcodec()
{
	if (ctx)
	{
		avcodec_free_context(&ctx);
	}

	if (!is_valid())
	{
		return false;
	}

	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
	{
		cellAtracXdec.error("avcodec_alloc_context3() failed");
		return false;
	}

	// FFmpeg allocates the frames itself. The decoded samples are converted into the output buffer in guest memory afterwards
	ctx->block_align = nbytes;
	ctx->ch_layout.nb_channels = nch_in;
	ctx->sample_rate = sampling_freq;

	if (int err = avcodec_open2(ctx, codec, nullptr); err)
	{
		cellAtracXdec.error("avcodec_open2() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
		avcodec_free_context(&ctx);
		return false;
	}

	return true;
}

error_code AtracXdecHost::set_config_info(u32 sampling_freq, u32 ch_config_idx, u32 nbytes)
{
	cellAtracXdec.notice("AtracXdecHost::set_config_info(sampling_freq=%d, ch_config_idx=%d, nbytes=0x%x)", sampling_freq, ch_config_idx, nbytes);

	// The new configuration is only applied if it is valid. FFmpeg has to be reinitialized with init_decode() either way
	config_is_set = false;
	decoder_ready = false;

	// These checks are performed on the LLE SPU thread, except for the number of channels supported by the work memory
	if (!atracxdec_internal::check_config_info(max_nch, sampling_freq, ch_config_idx, nbytes))
	{
		return { 0x80004005, "AtracXdecHost::set_config_info() failed: Invalid configuration (sampling_freq=%d, ch_config_idx=%d, nbytes=0x%x, max_nch=%d)", sampling_freq, ch_config_idx, nbytes, max_nch };
	}

	this->sampling_freq = sampling_freq;
	this->ch_config_idx = ch_config_idx;
	this->nch_in = atracxdec_internal::get_nch_in(ch_config_idx);
	this->nbytes = nbytes;
	this->config_is_set = true;
	return CELL_OK;
}

error_code AtracXdecHost::init_decode(u32 bw_pcm, u32 nch_out)
{
	decoder_ready = false;

	if (!config_is_set)
	{
		return { 0x80004005, "AtracXdecHost::init_decode() failed: The configuration is not set" };
	}

	if (!atracxdec_internal::check_output_format(max_nch, bw_pcm, nch_out))
	{
		return { 0x80004005, "AtracXdecHost::init_decode() failed: Invalid PCM output format (bw_pcm=0x%x, nch_out=%d, max_nch=%d)", bw_pcm, nch_out, max_nch };
	}

	if (nch_out != nch_in)
	{
		cellAtracXdec.warning("AtracXdecHost::init_decode(): nch_out (%d) != nch_in (%d), downmixing is not implemented", nch_out, nch_in);
	}

	this->bw_pcm = bw_pcm;
	this->nch_out = nch_out;
	this->pcm_output_size = atracxdec_internal::get_pcm_output_size(bw_pcm, nch_in);

	if (!init_avcodec())
	{
		return { 0x80004005, "AtracXdecHost::init_decode() failed: FFmpeg initialization failed" };
	}

	decoder_ready = true;
	return CELL_OK;
}

error_code AtracXdecHost::parse_ats_header(u32 au_start_addr)
{
	if (!vm::check_addr(au_start_addr, vm::page_readable, sizeof(AtracXdecAtsHeader)))
	{
		return { CELL_ADEC_ERROR_ATX_ATSHDR, "AtracXdecHost::parse_ats_header() failed: Invalid address: 0x%x", au_start_addr };
	}

	const auto ats = std::bit_cast<AtracXdecAtsHeader>(vm::read64(au_start_addr));
	const auto info = atracxdec_internal::parse_ats_header(ats);

	if (!info)
	{
		return { CELL_ADEC_ERROR_ATX_ATSHDR, "AtracXdecHost::parse_ats_header() failed: Invalid ATS header (sync_word=0x%x, params=0x%x)", ats.sync_word, ats.params };
	}

	// The values were already checked, except for the number of channels supported by the work memory
	if (set_config_info(info->sampling_freq, info->ch_config_idx, info->nbytes) != CELL_OK)
	{
		return { CELL_ADEC_ERROR_ATX_ATSHDR, "AtracXdecHost::parse_ats_header() failed: Unsupported channel configuration: %d", info->ch_config_idx };
	}

	return CELL_OK;
}

u32 AtracXdecHost::decode(u32 input_addr, u32& error)
{
	std::memcpy(input.data(), vm::base(input_addr), nbytes);

	// FFmpeg copies the data since the packet isn't reference counted
	packet->data = input.data();
	packet->size = static_cast<int>(nbytes);

	if (int err = avcodec_send_packet(ctx, packet); err)
	{
		// These errors should never occur
		if (err == AVERROR(EAGAIN) ||
			err == averror_eof ||
			err == AVERROR(EINVAL) ||
			err == AVERROR(ENOMEM))
		{
			fmt::throw_exception("avcodec_send_packet() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
		}

		// Game sent invalid data
		cellAtracXdec.error("avcodec_send_packet() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
		error = CELL_ADEC_ERROR_ATX_NON_FATAL; // Not accurate, FFmpeg doesn't provide detailed errors like LLE

		av_frame_unref(frame);
		return 0;
	}

	if (int err = avcodec_receive_frame(ctx, frame); err)
	{
		if (err == AVERROR(ENOMEM))
		{
			fmt::throw_exception("avcodec_receive_frame() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
		}

		cellAtracXdec.error("avcodec_receive_frame() failed (err=0x%x='%s')", err, utils::av_error_to_string(err));
		error = CELL_ADEC_ERROR_ATX_NON_FATAL;

		av_frame_unref(frame);
		return 0;
	}

	const u32 nb_samples = static_cast<u32>(frame->nb_samples);

	if (nb_samples == 0u)
	{
		return 0;
	}

	// The output conversion expects one full frame of planar float samples for each input channel
	bool valid = nb_samples == ATXDEC_SAMPLES_PER_FRAME && frame->format == AV_SAMPLE_FMT_FLTP && frame->ch_layout.nb_channels == static_cast<int>(nch_in) && frame->extended_data;

	for (u32 i = 0; valid && i < nch_in; i++)
	{
		valid = !!frame->extended_data[i];
	}

	if (!valid)
	{
		cellAtracXdec.error("Unexpected frame from FFmpeg (nb_samples=%d, format=%d, nb_channels=%d, nch_in=%d)", frame->nb_samples, frame->format, frame->ch_layout.nb_channels, nch_in);
		error = CELL_ADEC_ERROR_ATX_FATAL;

		av_frame_unref(frame);
		return 0;
	}

	return nb_samples;
}

void AtracXdecContext::exec(ppu_thread& ppu, AtracXdecHost& host)
{
	perf_meter<"ATXDEC"_u64> perf0;

	switch (savestate)
	{
	case atracxdec_state::initial: break;
	case atracxdec_state::waiting_for_cmd: goto label1_wait_for_cmd_state;
	case atracxdec_state::checking_run_thread_1: goto label2_check_run_thread_1_state;
	case atracxdec_state::executing_cmd: goto label3_execute_cmd_state;
	case atracxdec_state::waiting_for_output: goto label4_wait_for_output_state;
	case atracxdec_state::checking_run_thread_2: goto label5_check_run_thread_2_state;
	case atracxdec_state::decoding: goto label6_decode_state;
	}

	for (;;cmd_counter++)
	{
		cellAtracXdec.trace("Command counter: %llu, waiting for next command...", cmd_counter);

		for (;;)
		{
			savestate = atracxdec_state::initial;

			ensure(lv2_syscall<sys_mutex_lock>(ppu, queue_mutex, 0) == CELL_OK);

			if (ppu.state & cpu_flag::again)
			{
				return;
			}

			if (cmd_queue.pop(cmd))
			{
				break;
			}

			savestate = atracxdec_state::waiting_for_cmd;
			label1_wait_for_cmd_state:

			ensure(lv2_syscall<sys_cond_wait>(ppu, queue_not_empty, 0) == CELL_OK);

			if (ppu.state & cpu_flag::again)
			{
				return;
			}

			ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);
		}

		ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);

		savestate = atracxdec_state::checking_run_thread_1;
		label2_check_run_thread_1_state:

		ensure(lv2_syscall<sys_mutex_lock>(ppu, run_thread_mutex, 0) == CELL_OK);

		if (ppu.state & cpu_flag::again)
		{
			return;
		}

		if (!run_thread || host.closed)
		{
			ensure(lv2_syscall<sys_mutex_unlock>(ppu, run_thread_mutex) == CELL_OK);
			return;
		}

		ensure(lv2_syscall<sys_mutex_unlock>(ppu, run_thread_mutex) == CELL_OK);

		savestate = atracxdec_state::executing_cmd;
		label3_execute_cmd_state:

		cellAtracXdec.trace("Command type: %d", static_cast<u32>(cmd.type.get()));

		switch (cmd.type)
		{
		case AtracXdecCmdType::start_seq:
		{
			first_decode = true;
			skip_next_frame = true;

			// Skip if access units contain an ATS header, the parameters are included in the header and we need to wait for the first decode command to parse them
			if (cmd.atracx_param.au_includes_ats_hdr_flg == CELL_ADEC_ATRACX_ATS_HDR_NOTINC)
			{
				if (host.set_config_info(cmd.atracx_param.sampling_freq, cmd.atracx_param.ch_config_idx, cmd.atracx_param.nbytes) == static_cast<s32>(0x80004005))
				{
					break;
				}

				if (host.init_decode(cmd.atracx_param.bw_pcm, cmd.atracx_param.nch_out) == static_cast<s32>(0x80004005))
				{
					break;
				}
			}

			atracx_param = cmd.atracx_param;
			break;
		}
		case AtracXdecCmdType::end_seq:
		{
			// Block savestate creation during callbacks
			std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

			if (!savestate_lock.owns_lock())
			{
				ppu.state += cpu_flag::again;
				return;
			}

			// Doesn't do anything else
			notify_seq_done.cbFunc(ppu, notify_seq_done.cbArg);
			break;
		}
		case AtracXdecCmdType::decode_au:
		{
			cellAtracXdec.trace("Waiting for output to be consumed...");

			ensure(lv2_syscall<sys_mutex_lock>(ppu, output_mutex, 0) == CELL_OK);

			if (ppu.state & cpu_flag::again)
			{
				return;
			}

			while (output_locked)
			{
				savestate = atracxdec_state::waiting_for_output;
				label4_wait_for_output_state:

				ensure(lv2_syscall<sys_cond_wait>(ppu, output_consumed, 0) == CELL_OK);

				if (ppu.state & cpu_flag::again)
				{
					return;
				}
			}

			cellAtracXdec.trace("Output consumed");

			savestate = atracxdec_state::checking_run_thread_2;
			label5_check_run_thread_2_state:

			ensure(lv2_syscall<sys_mutex_lock>(ppu, run_thread_mutex, 0) == CELL_OK);

			if (ppu.state & cpu_flag::again)
			{
				return;
			}

			if (!run_thread || host.closed)
			{
				ensure(lv2_syscall<sys_mutex_unlock>(ppu, run_thread_mutex) == CELL_OK);
				ensure(lv2_syscall<sys_mutex_unlock>(ppu, output_mutex) == CELL_OK);
				return;
			}

			ensure(lv2_syscall<sys_mutex_unlock>(ppu, run_thread_mutex) == CELL_OK);

			savestate = atracxdec_state::decoding;
			label6_decode_state:

			u32 error = CELL_OK;

			// The parameters and the command are located in guest memory, so they are only read once
			const bool ats_hdr_included = atracx_param.au_includes_ats_hdr_flg == CELL_ADEC_ATRACX_ATS_HDR_INC;
			const u32 au_start_addr = cmd.au_start_addr.addr();

			// Only the first valid ATS header after starting a sequence is parsed. It is ignored on all subsequent access units
			if (first_decode && ats_hdr_included)
			{
				// Block savestate creation during callbacks
				std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

				if (!savestate_lock.owns_lock())
				{
					ppu.state += cpu_flag::again;
					return;
				}

				if (error = host.parse_ats_header(au_start_addr); error != CELL_OK)
				{
					notify_error.cbFunc(ppu, error, notify_error.cbArg);
				}
				else if (host.init_decode(atracx_param.bw_pcm, atracx_param.nch_out) != CELL_OK)
				{
					notify_error.cbFunc(ppu, CELL_ADEC_ERROR_ATX_FATAL, notify_error.cbArg);
				}
			}

			// LLE does not initialize the output address if parsing the ATS header fails
			vm::ptr<void> output = vm::null;
			u32 decoded_samples_num = 0;

			if (error != CELL_ADEC_ERROR_ATX_ATSHDR)
			{
				if (!host.decoder_ready)
				{
					// The LLE SPU thread would crash if you attempt to decode without a valid configuration
					cellAtracXdec.error("Attempted to decode without a valid configuration");
					error = CELL_ADEC_ERROR_ATX_FATAL;
				}
				else
				{
					// The output buffer is located in the work memory, which was checked in _CellAdecCoreOpOpenExt_atracx()
					output.set(host.work_mem_addr + atracxdec_internal::get_spurs_mem_size(host.nch_in));

					const u64 input_addr = u64{au_start_addr} + (ats_hdr_included ? u64{sizeof(AtracXdecAtsHeader)} : 0);

					// LLE doesn't check the access unit, and it ignores the size set by the user
					if (!vm::check_addr(input_addr, vm::page_readable, host.nbytes))
					{
						cellAtracXdec.error("Invalid access unit (au_start_addr=0x%x, nbytes=0x%x)", au_start_addr, host.nbytes);
						error = CELL_ADEC_ERROR_ATX_NON_FATAL;
					}
					else
					{
						decoded_samples_num = host.decode(static_cast<u32>(input_addr), error);
					}

					// The first frame after starting a new sequence or after an error is replaced with silence
					if (skip_next_frame && error == CELL_OK)
					{
						skip_next_frame = false;
						decoded_samples_num = 0;
						std::memset(output.get_ptr(), 0, ATXDEC_SAMPLES_PER_FRAME * (host.bw_pcm & 0x7fu) * host.nch_out);
					}

					// Convert FFmpeg output to LLE output
					if (decoded_samples_num != 0u)
					{
						const auto output_f32 = vm::static_ptr_cast<f32>(output).get_ptr();
						const auto output_s16 = vm::static_ptr_cast<s16>(output).get_ptr();
						const auto output_s32 = vm::static_ptr_cast<s32>(output).get_ptr();
						const u8* const ch_map = ATXDEC_AVCODEC_CH_MAP[host.ch_config_idx - 1];
						const u32 nch_in = host.nch_in;

						switch (host.bw_pcm)
						{
						case CELL_ADEC_ATRACX_WORD_SZ_FLOAT:
							for (u32 channel_idx = 0; channel_idx < nch_in; channel_idx++)
							{
								const f32* samples = host.get_samples(channel_idx);

								for (u32 in_sample_idx = 0, out_sample_idx = ch_map[channel_idx]; in_sample_idx < decoded_samples_num; in_sample_idx++, out_sample_idx += nch_in)
								{
									const f32 sample = samples[in_sample_idx];

									if (sample >= std::bit_cast<f32>(std::bit_cast<u32>(1.f) - 1))
									{
										output_f32[out_sample_idx] = std::bit_cast<be_t<f32>>("\x3f\x7f\xff\xff"_u32); // Prevents an unnecessary endian swap
									}
									else if (sample <= -1.f)
									{
										output_f32[out_sample_idx] = -1.f;
									}
									else
									{
										output_f32[out_sample_idx] = sample;
									}
								}
							}
							break;

						case CELL_ADEC_ATRACX_WORD_SZ_16BIT:
							for (u32 channel_idx = 0; channel_idx < nch_in; channel_idx++)
							{
								const f32* samples = host.get_samples(channel_idx);

								for (u32 in_sample_idx = 0, out_sample_idx = ch_map[channel_idx]; in_sample_idx < decoded_samples_num; in_sample_idx++, out_sample_idx += nch_in)
								{
									const f32 sample = samples[in_sample_idx];

									if (sample >= 1.f)
									{
										output_s16[out_sample_idx] = INT16_MAX;
									}
									else if (sample <= -1.f)
									{
										output_s16[out_sample_idx] = INT16_MIN;
									}
									else
									{
										output_s16[out_sample_idx] = static_cast<s16>(std::floor(sample * 0x8000u));
									}
								}
							}
							break;

						case CELL_ADEC_ATRACX_WORD_SZ_24BIT:
							for (u32 channel_idx = 0; channel_idx < nch_in; channel_idx++)
							{
								const f32* samples = host.get_samples(channel_idx);

								for (u32 in_sample_idx = 0, out_sample_idx = ch_map[channel_idx]; in_sample_idx < decoded_samples_num; in_sample_idx++, out_sample_idx += nch_in)
								{
									const f32 sample = samples[in_sample_idx];

									if (sample >= 1.f)
									{
										output_s32[out_sample_idx] = 0x007fffff;
									}
									else if (sample <= -1.f)
									{
										output_s32[out_sample_idx] = 0x00800000;
									}
									else
									{
										output_s32[out_sample_idx] = static_cast<s32>(std::floor(sample * 0x00800000u)) & 0x00ffffff;
									}
								}
							}
							break;

						case CELL_ADEC_ATRACX_WORD_SZ_32BIT:
							for (u32 channel_idx = 0; channel_idx < nch_in; channel_idx++)
							{
								const f32* samples = host.get_samples(channel_idx);

								for (u32 in_sample_idx = 0, out_sample_idx = ch_map[channel_idx]; in_sample_idx < decoded_samples_num; in_sample_idx++, out_sample_idx += nch_in)
								{
									const f32 sample = samples[in_sample_idx];

									if (sample >= 1.f)
									{
										output_s32[out_sample_idx] = INT32_MAX;
									}
									else if (sample <= -1.f)
									{
										output_s32[out_sample_idx] = INT32_MIN;
									}
									else
									{
										output_s32[out_sample_idx] = static_cast<s32>(std::floor(sample * 0x80000000u));
									}
								}
							}
						}
					}
				}

				first_decode = false;

				if (error != CELL_OK)
				{
					// Block savestate creation during callbacks
					std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

					if (!savestate_lock.owns_lock())
					{
						ppu.state += cpu_flag::again;
						return;
					}

					skip_next_frame = true;
					notify_error.cbFunc(ppu, error, notify_error.cbArg);
				}
			}

			// Block savestate creation during callbacks
			std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

			if (!savestate_lock.owns_lock())
			{
				ppu.state += cpu_flag::again;
				return;
			}

			// au_done and pcm_out callbacks are always called after a decode command, even if an error occurred
			// The output always has to be consumed as well
			notify_au_done.cbFunc(ppu, cmd.pcm_handle, notify_au_done.cbArg);

			output_locked = true;
			ensure(lv2_syscall<sys_mutex_unlock>(ppu, output_mutex) == CELL_OK);

			const u32 output_size = decoded_samples_num * (host.bw_pcm & 0x7fu) * host.nch_out;

			const vm::var<CellAdecAtracXInfo> bsi_info{{ host.sampling_freq, host.ch_config_idx, host.nbytes }};

			const AdecCorrectPtsValueType correct_pts_type = [&]
			{
				switch (host.sampling_freq)
				{
				case 32000u: return ADEC_CORRECT_PTS_VALUE_TYPE_ATRACX_32000Hz;
				case 44100u: return ADEC_CORRECT_PTS_VALUE_TYPE_ATRACX_44100Hz;
				case 48000u: return ADEC_CORRECT_PTS_VALUE_TYPE_ATRACX_48000Hz;
				default:     return ADEC_CORRECT_PTS_VALUE_TYPE_UNSPECIFIED;
				}
			}();

			notify_pcm_out.cbFunc(ppu, cmd.pcm_handle, output, output_size, notify_pcm_out.cbArg, vm::make_var<vm::bcptr<void>>(bsi_info), correct_pts_type, error);
			break;
		}
		default:
		{
			// The guest overwrote the command
			std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

			if (!savestate_lock.owns_lock())
			{
				ppu.state += cpu_flag::again;
				return;
			}

			cellAtracXdec.error("AtracXdecContext::exec(): Invalid command type: 0x%x", static_cast<u32>(cmd.type.get()));

			notify_error.cbFunc(ppu, CELL_ADEC_ERROR_ATX_FATAL, notify_error.cbArg);
			break;
		}
		}
	}
}

template <AtracXdecCmdType type>
error_code AtracXdecContext::send_command(ppu_thread& ppu, auto&&... args)
{
	auto& savestate = *ppu.optional_savestate_state;
	const bool signal = savestate.try_read<bool>().second;
	savestate.clear();

	if (!signal)
	{
		ensure(lv2_syscall<sys_mutex_lock>(ppu, queue_mutex, 0) == CELL_OK);

		if (ppu.state & cpu_flag::again)
		{
			return {};
		}

		if constexpr (type == AtracXdecCmdType::close)
		{
			// Close command is only sent if the queue is empty on LLE
			if (!cmd_queue.empty())
			{
				ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);
				return {};
			}
		}

		if (!cmd_queue.emplace(std::forward<AtracXdecCmdType>(type), std::forward<decltype(args)>(args)...))
		{
			ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);
			return CELL_ADEC_ERROR_ATX_BUSY;
		}

		ensure(lv2_syscall<sys_mutex_unlock>(ppu, queue_mutex) == CELL_OK);
	}

	ensure(lv2_syscall<sys_cond_signal>(ppu, queue_not_empty) == CELL_OK);

	if (ppu.state & cpu_flag::again)
	{
		savestate(true);
	}

	return CELL_OK;
}

void atracXdecEntry(ppu_thread& ppu, vm::ptr<AtracXdecContext> atxdec)
{
	const auto host = atracxdec_internal::get_host(atxdec);

	// The host object is bound to the first thread running this function, so that the FFmpeg decoder is never used by several threads if the guest calls this function directly
	if (!host || !host->bind_worker(ppu.id))
	{
		cellAtracXdec.error("atracXdecEntry(): Invalid decoder (atxdec=*0x%x)", atxdec);
		ppu_execute<&sys_ppu_thread_exit>(ppu, CELL_OK);
		return;
	}

	atxdec->exec(ppu, *host);

	if (ppu.state & cpu_flag::again)
	{
		// For savestates, save argument
		ppu.syscall_args[0] = atxdec.addr();

		return;
	}

	ppu_execute<&sys_ppu_thread_exit>(ppu, CELL_OK);
}

template <u32 nch_in>
error_code _CellAdecCoreOpGetMemSize_atracx(vm::ptr<CellAdecAttr> attr)
{
	cellAtracXdec.notice("_CellAdecCoreOpGetMemSize_atracx<nch_in=%d>(attr=*0x%x)", nch_in, attr);

	ensure(!!attr); // Not checked on LLE

	constexpr u32 mem_size =
		sizeof(AtracXdecContext) + 0x7f
		+ ATXDEC_SPURS_STRUCTS_SIZE + 0x1d8
		+ atracxdec_internal::get_spurs_mem_size(nch_in)
		+ ATXDEC_SAMPLES_PER_FRAME * sizeof(f32) * nch_in;

	// The memory accessed by HLE fits even if the context had to be aligned by cellAdec
	static_assert(atracxdec_internal::get_work_mem_end(nch_in) <= mem_size - 0x7f);

	attr->workMemSize = utils::align(mem_size, 0x80);

	return CELL_OK;
}

template <u32 max_nch>
error_code _CellAdecCoreOpOpenExt_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, vm::ptr<AdecNotifyAuDone> notifyAuDone, vm::ptr<void> notifyAuDoneArg, vm::ptr<AdecNotifyPcmOut> notifyPcmOut, vm::ptr<void> notifyPcmOutArg,
	vm::ptr<AdecNotifyError> notifyError, vm::ptr<void> notifyErrorArg, vm::ptr<AdecNotifySeqDone> notifySeqDone, vm::ptr<void> notifySeqDoneArg, vm::cptr<CellAdecResource> res, vm::cptr<CellAdecResourceSpurs> spursRes)
{
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	cellAtracXdec.notice("_CellAdecCoreOpOpenExt_atracx<max_nch=%d>(handle=*0x%x, notifyAuDone=*0x%x, notifyAuDoneArg=*0x%x, notifyPcmOut=*0x%x, notifyPcmOutArg=*0x%x, notifyError=*0x%x, notifyErrorArg=*0x%x, notifySeqDone=*0x%x, notifySeqDoneArg=*0x%x, res=*0x%x, spursRes=*0x%x)",
		max_nch, handle, notifyAuDone, notifyAuDoneArg, notifyPcmOut, notifyPcmOutArg, notifyError, notifyErrorArg, notifySeqDone, notifySeqDoneArg, res, spursRes);

	// None of these are checked on LLE. cellAdec always sets the callbacks and aligns the address to 128 bytes
	if (!handle || !res || !handle.aligned(0x80) || !notifyAuDone || !notifyAuDoneArg || !notifyPcmOut || !notifyPcmOutArg || !notifyError || !notifyErrorArg || !notifySeqDone || !notifySeqDoneArg)
	{
		return CELL_ADEC_ERROR_ARG;
	}

	// The work memory has to be accessible for the maximum number of channels of this decoder type, HLE never accesses memory past it
	if (!vm::check_addr(handle.addr(), vm::page_readable | vm::page_writable, atracxdec_internal::get_work_mem_end(max_nch)))
	{
		cellAtracXdec.error("_CellAdecCoreOpOpenExt_atracx(): Invalid work memory (handle=*0x%x, size=0x%x)", handle, atracxdec_internal::get_work_mem_end(max_nch));
		return CELL_ADEC_ERROR_FATAL;
	}

	// If the guest reused the memory of a decoder without closing it, the old host object is retired.
	// Its thread can't access the new host object, since it is bound to the old one
	if (const auto [old_id, old_host] = atracxdec_internal::find_host(handle.addr()); old_host)
	{
		cellAtracXdec.warning("_CellAdecCoreOpOpenExt_atracx(): The decoder at *0x%x was not closed", handle);

		old_host->closed = true;
		idm::remove_verify<AtracXdecHost>(old_id, old_host);
	}

	const u32 host_id = idm::make<AtracXdecHost>(handle.addr(), max_nch);
	const auto host = idm::get_unlocked<AtracXdecHost>(host_id);

	if (!host || !host->is_valid())
	{
		if (host)
		{
			idm::remove_verify<AtracXdecHost>(host_id, host);
		}

		return CELL_ADEC_ERROR_FATAL;
	}

	// Clear the context first, the padding bytes are not initialized by the constructor
	std::memset(handle.get_ptr(), 0, sizeof(AtracXdecContext));
	new (handle.get_ptr()) AtracXdecContext(notifyAuDone, notifyAuDoneArg, notifyPcmOut, notifyPcmOutArg, notifyError, notifyErrorArg, notifySeqDone, notifySeqDoneArg,
		vm::bptr<u8>::make(handle.addr() + atracxdec_internal::WORK_MEM_OFFSET), host_id);

	const vm::var<sys_mutex_attribute_t> mutex_attr{{ SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE, SYS_SYNC_NOT_PROCESS_SHARED, SYS_SYNC_NOT_ADAPTIVE, 0, 0, 0, { "_atd001"_u64 } }};
	const vm::var<sys_cond_attribute_t> cond_attr{{ SYS_SYNC_NOT_PROCESS_SHARED, 0, 0, { "_atd002"_u64 } }};

	ensure(lv2_syscall<sys_mutex_create>(ppu, handle.ptr(&AtracXdecContext::queue_mutex), mutex_attr) == CELL_OK);
	ensure(lv2_syscall<sys_cond_create>(ppu, handle.ptr(&AtracXdecContext::queue_not_empty), handle->queue_mutex, cond_attr) == CELL_OK);

	mutex_attr->name_u64 = "_atd003"_u64;
	cond_attr->name_u64 = "_atd004"_u64;

	ensure(lv2_syscall<sys_mutex_create>(ppu, handle.ptr(&AtracXdecContext::run_thread_mutex), mutex_attr) == CELL_OK);
	ensure(lv2_syscall<sys_cond_create>(ppu, handle.ptr(&AtracXdecContext::run_thread_cond), handle->run_thread_mutex, cond_attr) == CELL_OK);

	mutex_attr->name_u64 = "_atd005"_u64;
	cond_attr->name_u64 = "_atd006"_u64;

	ensure(lv2_syscall<sys_mutex_create>(ppu, handle.ptr(&AtracXdecContext::output_mutex), mutex_attr) == CELL_OK);
	ensure(lv2_syscall<sys_cond_create>(ppu, handle.ptr(&AtracXdecContext::output_consumed), handle->output_mutex, cond_attr) == CELL_OK);

	ensure(lv2_syscall<sys_mutex_lock>(ppu, handle->output_mutex, 0) == CELL_OK);
	handle->output_locked = false;
	ensure(lv2_syscall<sys_cond_signal>(ppu, handle->output_consumed) == CELL_OK);
	ensure(lv2_syscall<sys_mutex_unlock>(ppu, handle->output_mutex) == CELL_OK);

	const vm::var<char[]> _name = vm::make_str("HLE ATRAC3plus decoder");
	const auto entry = g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(atracXdecEntry));

	if (error_code ret = ppu_execute<&sys_ppu_thread_create>(ppu, handle.ptr(&AtracXdecContext::thread_id), entry, handle.addr(), +res->ppuThreadPriority, +res->ppuThreadStackSize, SYS_PPU_THREAD_CREATE_JOINABLE, +_name); ret != CELL_OK)
	{
		host->closed = true;
		idm::remove_verify<AtracXdecHost>(host_id, host);
		return ret;
	}

	return CELL_OK;
}

template <u32 max_nch>
error_code _CellAdecCoreOpOpen_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, vm::ptr<AdecNotifyAuDone> notifyAuDone, vm::ptr<void> notifyAuDoneArg, vm::ptr<AdecNotifyPcmOut> notifyPcmOut, vm::ptr<void> notifyPcmOutArg,
	vm::ptr<AdecNotifyError> notifyError, vm::ptr<void> notifyErrorArg, vm::ptr<AdecNotifySeqDone> notifySeqDone, vm::ptr<void> notifySeqDoneArg, vm::cptr<CellAdecResource> res)
{
	cellAtracXdec.notice("_CellAdecCoreOpOpen_atracx<max_nch=%d>(handle=*0x%x, notifyAuDone=*0x%x, notifyAuDoneArg=*0x%x, notifyPcmOut=*0x%x, notifyPcmOutArg=*0x%x, notifyError=*0x%x, notifyErrorArg=*0x%x, notifySeqDone=*0x%x, notifySeqDoneArg=*0x%x, res=*0x%x)",
		max_nch, handle, notifyAuDone, notifyAuDoneArg, notifyPcmOut, notifyPcmOutArg, notifyError, notifyErrorArg, notifySeqDone, notifySeqDoneArg, res);

	return _CellAdecCoreOpOpenExt_atracx<max_nch>(ppu, handle, notifyAuDone, notifyAuDoneArg, notifyPcmOut, notifyPcmOutArg, notifyError, notifyErrorArg, notifySeqDone, notifySeqDoneArg, res, vm::null);
}

error_code _CellAdecCoreOpClose_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle)
{
	std::unique_lock savestate_lock{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!savestate_lock.owns_lock())
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	cellAtracXdec.notice("_CellAdecCoreOpClose_atracx(handle=*0x%x)", handle);

	if (!handle) // Not checked on LLE
	{
		return CELL_ADEC_ERROR_ARG;
	}

	// The host object is looked up by the address of the context, so that it is also found if the guest overwrote the ID
	const auto [host_id, host] = atracxdec_internal::find_host(handle.addr());

	if (!host || host->closed.exchange(true))
	{
		return CELL_ADEC_ERROR_ATX_FATAL_HANDLE;
	}

	ensure(lv2_syscall<sys_mutex_lock>(ppu, handle->run_thread_mutex, 0) == CELL_OK);
	handle->run_thread = false;
	ensure(lv2_syscall<sys_mutex_unlock>(ppu, handle->run_thread_mutex) == CELL_OK);

	handle->send_command<AtracXdecCmdType::close>(ppu);

	ensure(lv2_syscall<sys_mutex_lock>(ppu, handle->output_mutex, 0) == CELL_OK);
	handle->output_locked = false;
	ensure(lv2_syscall<sys_mutex_unlock>(ppu, handle->output_mutex) == CELL_OK);
	ensure(lv2_syscall<sys_cond_signal>(ppu, handle->output_consumed) == CELL_OK);

	vm::var<u64> thread_ret;
	ensure(lv2_syscall<sys_ppu_thread_join>(ppu, static_cast<u32>(handle->thread_id), +thread_ret) == CELL_OK);

	// The decoder thread has exited, the FFmpeg decoder is freed once the last reference is released
	idm::remove_verify<AtracXdecHost>(host_id, host);

	error_code ret = lv2_syscall<sys_cond_destroy>(ppu, handle->queue_not_empty);
	ret = ret ? ret : lv2_syscall<sys_cond_destroy>(ppu, handle->run_thread_cond);
	ret = ret ? ret : lv2_syscall<sys_cond_destroy>(ppu, handle->output_consumed);
	ret = ret ? ret : lv2_syscall<sys_mutex_destroy>(ppu, handle->queue_mutex);
	ret = ret ? ret : lv2_syscall<sys_mutex_destroy>(ppu, handle->run_thread_mutex);
	ret = ret ? ret : lv2_syscall<sys_mutex_destroy>(ppu, handle->output_mutex);

	return ret != CELL_OK ? static_cast<error_code>(CELL_ADEC_ERROR_FATAL) : CELL_OK;
}

error_code _CellAdecCoreOpStartSeq_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, vm::cptr<CellAdecParamAtracX> atracxParam)
{
	cellAtracXdec.notice("_CellAdecCoreOpStartSeq_atracx(handle=*0x%x, atracxParam=*0x%x)", handle, atracxParam);

	if (!atracxdec_internal::get_host(handle))
	{
		return CELL_ADEC_ERROR_ATX_FATAL_HANDLE;
	}

	if (!atracxParam) // Not checked on LLE
	{
		return CELL_ADEC_ERROR_ARG;
	}

	cellAtracXdec.notice("_CellAdecCoreOpStartSeq_atracx(): sampling_freq=%d, ch_config_idx=%d, nch_out=%d, nbytes=0x%x, extra_config_data=0x%08x, bw_pcm=0x%x, downmix_flag=%d, au_includes_ats_hdr_flg=%d",
		atracxParam->sampling_freq, atracxParam->ch_config_idx, atracxParam->nch_out, atracxParam->nbytes, std::bit_cast<u32>(atracxParam->extra_config_data), atracxParam->bw_pcm, atracxParam->downmix_flag, atracxParam->au_includes_ats_hdr_flg);

	return handle->send_command<AtracXdecCmdType::start_seq>(ppu, *atracxParam);
}

error_code _CellAdecCoreOpEndSeq_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle)
{
	cellAtracXdec.notice("_CellAdecCoreOpEndSeq_atracx(handle=*0x%x)", handle);

	if (!atracxdec_internal::get_host(handle))
	{
		return CELL_ADEC_ERROR_ATX_FATAL_HANDLE;
	}

	return handle->send_command<AtracXdecCmdType::end_seq>(ppu);
}

error_code _CellAdecCoreOpDecodeAu_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, s32 pcmHandle, vm::cptr<CellAdecAuInfo> auInfo)
{
	cellAtracXdec.trace("_CellAdecCoreOpDecodeAu_atracx(handle=*0x%x, pcmHandle=%d, auInfo=*0x%x)", handle, pcmHandle, auInfo);

	if (!atracxdec_internal::get_host(handle))
	{
		return CELL_ADEC_ERROR_ATX_FATAL_HANDLE;
	}

	if (!auInfo) // Not checked on LLE
	{
		return CELL_ADEC_ERROR_ARG;
	}

	cellAtracXdec.trace("_CellAdecCoreOpDecodeAu_atracx(): startAddr=*0x%x, size=0x%x, pts=%lld, userData=0x%llx", auInfo->startAddr, auInfo->size, std::bit_cast<be_t<u64>>(auInfo->pts), auInfo->userData);

	return handle->send_command<AtracXdecCmdType::decode_au>(ppu, pcmHandle, *auInfo);
}

void _CellAdecCoreOpGetVersion_atracx(vm::ptr<be_t<u32, 1>> version)
{
	cellAtracXdec.notice("_CellAdecCoreOpGetVersion_atracx(version=*0x%x)", version);

	ensure(!!version); // Not checked on LLE

	*version = 0x01020000;
}

error_code _CellAdecCoreOpRealign_atracx(vm::ptr<AtracXdecContext> handle, vm::ptr<void> outBuffer, vm::cptr<void> pcmStartAddr)
{
	cellAtracXdec.trace("_CellAdecCoreOpRealign_atracx(handle=*0x%x, outBuffer=*0x%x, pcmStartAddr=*0x%x)", handle, outBuffer, pcmStartAddr);

	if (outBuffer)
	{
		const auto host = atracxdec_internal::get_host(handle);

		if (!host)
		{
			return CELL_ADEC_ERROR_ATX_FATAL_HANDLE;
		}

		const u32 pcm_output_size = host->pcm_output_size;

		// Not checked on LLE
		if (!vm::check_addr(outBuffer.addr(), vm::page_writable, pcm_output_size) || !vm::check_addr(pcmStartAddr.addr(), vm::page_readable, pcm_output_size))
		{
			cellAtracXdec.error("_CellAdecCoreOpRealign_atracx(): Invalid buffer (outBuffer=*0x%x, pcmStartAddr=*0x%x, size=0x%x)", outBuffer, pcmStartAddr, pcm_output_size);
			return CELL_ADEC_ERROR_ARG;
		}

		std::memcpy(outBuffer.get_ptr(), pcmStartAddr.get_ptr(), pcm_output_size);
	}

	return CELL_OK;
}

error_code _CellAdecCoreOpReleasePcm_atracx(ppu_thread& ppu, vm::ptr<AtracXdecContext> handle, s32 pcmHandle, vm::cptr<void> outBuffer)
{
	cellAtracXdec.trace("_CellAdecCoreOpReleasePcm_atracx(handle=*0x%x, pcmHandle=%d, outBuffer=*0x%x)", handle, pcmHandle, outBuffer);

	if (!atracxdec_internal::get_host(handle))
	{
		return CELL_ADEC_ERROR_ATX_FATAL_HANDLE;
	}

	auto& savestate = *ppu.optional_savestate_state;
	const bool signal = savestate.try_read<bool>().second;
	savestate.clear();

	if (!signal)
	{
		ensure(lv2_syscall<sys_mutex_lock>(ppu, handle->output_mutex, 0) == CELL_OK);

		if (ppu.state & cpu_flag::again)
		{
			return {};
		}

		handle->output_locked = false;
	}

	ensure(lv2_syscall<sys_cond_signal>(ppu, handle->output_consumed) == CELL_OK);

	if (ppu.state & cpu_flag::again)
	{
		savestate(true);
		return {};
	}

	ensure(lv2_syscall<sys_mutex_unlock>(ppu, handle->output_mutex) == CELL_OK);

	return CELL_OK;
}

s32 _CellAdecCoreOpGetPcmHandleNum_atracx()
{
	cellAtracXdec.notice("_CellAdecCoreOpGetPcmHandleNum_atracx()");

	return 3;
}

u32 _CellAdecCoreOpGetBsiInfoSize_atracx()
{
	cellAtracXdec.notice("_CellAdecCoreOpGetBsiInfoSize_atracx()");

	return sizeof(CellAdecAtracXInfo);
}

// Each decoder type has its own open functions, since the size of the work memory depends on the maximum number of channels
template <u32 max_nch>
static void init_gvar(vm::gvar<CellAdecCoreOps>& var)
{
	var->getMemSize.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetMemSize_atracx<max_nch>)));
	var->open.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpOpen_atracx<max_nch>)));
	var->close.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpClose_atracx)));
	var->startSeq.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpStartSeq_atracx)));
	var->endSeq.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpEndSeq_atracx)));
	var->decodeAu.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpDecodeAu_atracx)));
	var->getVersion.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetVersion_atracx)));
	var->realign.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpRealign_atracx)));
	var->releasePcm.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpReleasePcm_atracx)));
	var->getPcmHandleNum.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetPcmHandleNum_atracx)));
	var->getBsiInfoSize.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpGetBsiInfoSize_atracx)));
	var->openExt.set(g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(_CellAdecCoreOpOpenExt_atracx<max_nch>)));
}

DECLARE(ppu_module_manager::cellAtracXdec)("cellAtracXdec", []()
{
	REG_VNID(cellAtracXdec, 0x076b33ab, g_cell_adec_core_ops_atracx2ch).init = []()
	{
		init_gvar<2>(g_cell_adec_core_ops_atracx2ch);
	};
	REG_VNID(cellAtracXdec, 0x1d210eaa, g_cell_adec_core_ops_atracx6ch).init = []()
	{
		init_gvar<6>(g_cell_adec_core_ops_atracx6ch);
	};
	REG_VNID(cellAtracXdec, 0xe9a86e54, g_cell_adec_core_ops_atracx8ch).init = []()
	{
		init_gvar<8>(g_cell_adec_core_ops_atracx8ch);
	};
	REG_VNID(cellAtracXdec, 0x4944af9a, g_cell_adec_core_ops_atracx).init = []()
	{
		init_gvar<8>(g_cell_adec_core_ops_atracx);
	};

	REG_HIDDEN_FUNC(_CellAdecCoreOpGetMemSize_atracx<2>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetMemSize_atracx<6>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetMemSize_atracx<8>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpOpen_atracx<2>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpOpen_atracx<6>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpOpen_atracx<8>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpClose_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpStartSeq_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpEndSeq_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpDecodeAu_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetVersion_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpRealign_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpReleasePcm_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetPcmHandleNum_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpGetBsiInfoSize_atracx);
	REG_HIDDEN_FUNC(_CellAdecCoreOpOpenExt_atracx<2>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpOpenExt_atracx<6>);
	REG_HIDDEN_FUNC(_CellAdecCoreOpOpenExt_atracx<8>);

	REG_HIDDEN_FUNC(atracXdecEntry);
});
