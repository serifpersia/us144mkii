// SPDX-License-Identifier: GPL-2.0-only

#include "us144mkii.h"
#include "controls.h"

static const char * const playback_source_texts[] = {"Playback 1-2", "Playback 3-4"};
static const char * const capture_source_texts[] = {"Analog In", "Digital In"};

static int tascam_playback_source_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 2;
	if (uinfo->value.enumerated.item >= 2)
		uinfo->value.enumerated.item = 1;
	strscpy(uinfo->value.enumerated.name,
		playback_source_texts[uinfo->value.enumerated.item],
		sizeof(uinfo->value.enumerated.name));
	return 0;
}

static int tascam_line_out_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = tascam->line_out_source;
	return 0;
}

static int tascam_line_out_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);

	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (tascam->line_out_source == ucontrol->value.enumerated.item[0])
		return 0;
	tascam->line_out_source = ucontrol->value.enumerated.item[0];
	return 1;
}

static const struct snd_kcontrol_new tascam_line_out_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Line OUTPUTS Source",
	.info = tascam_playback_source_info, .get = tascam_line_out_get, .put = tascam_line_out_put,
};

static int tascam_digital_out_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = tascam->digital_out_source;
	return 0;
}

static int tascam_digital_out_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);

	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (tascam->digital_out_source == ucontrol->value.enumerated.item[0])
		return 0;
	tascam->digital_out_source = ucontrol->value.enumerated.item[0];
	return 1;
}

static const struct snd_kcontrol_new tascam_digital_out_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Digital OUTPUTS Source",
	.info = tascam_playback_source_info, .get = tascam_digital_out_get, .put = tascam_digital_out_put,
};

static int tascam_capture_source_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 2;
	if (uinfo->value.enumerated.item >= 2)
		uinfo->value.enumerated.item = 1;
	strscpy(uinfo->value.enumerated.name,
		capture_source_texts[uinfo->value.enumerated.item],
		sizeof(uinfo->value.enumerated.name));
	return 0;
}

static int tascam_capture_12_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = tascam->capture_12_source;
	return 0;
}

static int tascam_capture_12_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);

	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (tascam->capture_12_source == ucontrol->value.enumerated.item[0])
		return 0;
	tascam->capture_12_source = ucontrol->value.enumerated.item[0];
	return 1;
}

static const struct snd_kcontrol_new tascam_capture_12_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "ch1 and ch2 Source",
	.info = tascam_capture_source_info, .get = tascam_capture_12_get, .put = tascam_capture_12_put,
};

static int tascam_capture_34_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = tascam->capture_34_source;
	return 0;
}

static int tascam_capture_34_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = snd_kcontrol_chip(kcontrol);

	if (ucontrol->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (tascam->capture_34_source == ucontrol->value.enumerated.item[0])
		return 0;
	tascam->capture_34_source = ucontrol->value.enumerated.item[0];
	return 1;
}

static const struct snd_kcontrol_new tascam_capture_34_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "ch3 and ch4 Source",
	.info = tascam_capture_source_info, .get = tascam_capture_34_get, .put = tascam_capture_34_put,
};

static int tascam_samplerate_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 96000;
	return 0;
}

static int tascam_samplerate_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct tascam_card *tascam = (struct tascam_card *)snd_kcontrol_chip(kcontrol);
	u8 *buf;
	int err;
	u32 rate = 0;

	if (tascam->current_rate > 0) {
		ucontrol->value.integer.value[0] = tascam->current_rate;
		return 0;
	}

	buf = kmalloc(3, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	err = usb_control_msg(tascam->dev, usb_rcvctrlpipe(tascam->dev, 0),
			      UAC_GET_CUR, RT_D2H_CLASS_EP,
			      UAC_SAMPLING_FREQ_CONTROL, EP_AUDIO_IN,
			      buf, 3, USB_CTRL_TIMEOUT_MS);

	if (err >= 3)
		rate = buf[0] | (buf[1] << 8) | (buf[2] << 16);

	ucontrol->value.integer.value[0] = rate;
	kfree(buf);
	return 0;
}

static const struct snd_kcontrol_new tascam_samplerate_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Sample Rate",
	.info = tascam_samplerate_info,
	.get = tascam_samplerate_get,
	.access = SNDRV_CTL_ELEM_ACCESS_READ,
};

int tascam_create_controls(struct tascam_card *tascam)
{
	int err;

	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_line_out_control, tascam));
	if (err < 0)
		return err;
	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_digital_out_control, tascam));
	if (err < 0)
		return err;
	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_capture_12_control, tascam));
	if (err < 0)
		return err;
	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_capture_34_control, tascam));
	if (err < 0)
		return err;

	err = snd_ctl_add(tascam->card, snd_ctl_new1(&tascam_samplerate_control, tascam));
	if (err < 0)
		return err;

	return 0;
}
