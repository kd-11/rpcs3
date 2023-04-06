R"(

#define MFC_TAG_UPDATE_IMMEDIATE  0
#define MFC_TAG_UPDATE_ANY        1
#define MFC_TAG_UPDATE_ALL        2

#define MFC_completed() (MFC_tag_mask & ~MFC_fence)

void MFC_tag_complete(const in int completed)
{
	MFC_tag_stat_value = completed;
	MFC_tag_stat_count = 1;
	MFC_tag_update = MFC_TAG_UPDATE_IMMEDIATE;
}

bool MFC_check_tag(const in int ch_tag_update, const in int completed)
{
	if ((ch_tag_update == MFC_TAG_UPDATE_ANY && completed != 0) ||
		(ch_tag_update == MFC_TAG_UPDATE_ALL && completed == MFC_tag_mask))
	{
		MFC_tag_complete(completed);
		return true;
	}

	return false;
}

void MFC_write_tag_mask()
{
	if (MFC_tag_update == MFC_TAG_UPDATE_IMMEDIATE)
	{
		return;
	}

	MFC_check_tag(MFC_tag_update, MFC_completed());
}

void MFC_write_tag_update(const in int value)
{
	if (value > MFC_TAG_UPDATE_ALL)
	{
		return;
	}

	const int completed = MFC_completed();
	if (value == MFC_TAG_UPDATE_IMMEDIATE)
	{
		MFC_tag_complete(completed);
		return;
	}

	if (!MFC_check_tag(value, completed))
	{
		// Failed. Defer.
		MFC_tag_update = value;
	}
}

void flush_LS(const in int offset, const in int length)
{
	const int start = ALIGN_DOWN(offset, 16);
	const int end = ALIGN_UP(offset + length, 16);
	const int word_offset = start >> 4;
	const int word_count = (end - start) >> 4;
	for (int i = word_offset; i < (word_offset + word_count); ++i)
	{
		ls_mirror[i] = ls[i];
	}
}

void MFC_cmd()
{
	// Flush LS
	flush_LS(MFC_lsa, MFC_size);

	// Would need direct access to system memory. Doable, but hard.
	exit_code = SPU_MFC_CMD;

	dr[0] = MFC_tag_mask;
	dr[1] = MFC_tag_stat_count;
	dr[2] = MFC_tag_stat_value;
	dr[3] = MFC_tag_update;
	dr[4] = MFC_tag_id;
	dr[5] = MFC_lsa;
	dr[6] = MFC_eal;
	dr[7] = MFC_eah;
	dr[8] = MFC_size;
	dr[9] = MFC_cmd_id;
	dr[10] = MFC_fence;
}
)"
