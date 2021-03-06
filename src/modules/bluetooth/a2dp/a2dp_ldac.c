
#include <arpa/inet.h>
#include <string.h>

#include "ldacBT.h"
#include "ldacBT_abr.h"

#include "../a2dp-api.h"

#include "ldac_libs.c"

#define streq(a, b) (!strcmp((a),(b)))

#define AVDT_MEDIA_HDR_SIZE 12

#define LDAC_ABR_THRESHOLD_CRITICAL 6
#define LDAC_ABR_THRESHOLD_DANGEROUSTREND 4
#define LDAC_ABR_THRESHOLD_SAFETY_FOR_HQSQ 2


typedef struct ldac_info {
    HANDLE_LDAC_BT hLdacBt;
    HANDLE_LDAC_ABR hLdacAbr;

    pa_a2dp_source_read_cb_t read_pcm;
    pa_a2dp_source_read_buf_free_cb_t read_buf_free;

    int eqmid;
    bool enable_abr;
    int channel_mode;
    pa_sample_format_t force_pa_fmt;
    LDACBT_SMPL_FMT_T pcm_fmt;
    uint32_t pcm_frequency;

    uint16_t pcm_lsu;
    size_t ldac_frame_size;
    size_t pcm_read_size;
    size_t q_write_block_size;
    pa_sample_spec sample_spec;

    uint16_t seq_num;
    uint32_t layer_specific;
    uint32_t written;
    size_t tx_length;

    size_t mtu;

} ldac_info_t;

static bool pa_ldac_encoder_load() {
    return ldac_encoder_load();
}

static bool
pa_ldac_encoder_init(pa_a2dp_source_read_cb_t read_cb, pa_a2dp_source_read_buf_free_cb_t free_cb, void **codec_data) {
    ldac_info_t *info = pa_xmalloc0(sizeof(ldac_info_t));
    *codec_data = info;
    info->read_pcm = read_cb;
    info->read_buf_free = free_cb;
    info->eqmid = LDACBT_EQMID_HQ;
    if(ldac_abr_loaded)
        info->enable_abr = true;
    info->force_pa_fmt = PA_SAMPLE_INVALID;
    return true;
}

static int pa_ldac_update_user_config(pa_proplist *user_config, void **codec_data) {
    ldac_info_t *i = *codec_data;
    const char *ldac_eqmid_str, *ldac_fmt_str;
    int ret = 0;
    ldac_eqmid_str = pa_proplist_gets(user_config, "ldac_eqmid");
    ldac_fmt_str = pa_proplist_gets(user_config, "ldac_fmt");

    pa_log_debug("LDAC ABR library loaded: %s",ldac_abr_loaded?"true":"false");

    if (ldac_eqmid_str) {
        if (streq(ldac_eqmid_str, "hq")) {
            i->eqmid = LDACBT_EQMID_HQ;
            i->enable_abr = false;
            ret++;
        } else if (streq(ldac_eqmid_str, "sq")) {
            i->eqmid = LDACBT_EQMID_SQ;
            i->enable_abr = false;
            ret++;
        } else if (streq(ldac_eqmid_str, "mq")) {
            i->eqmid = LDACBT_EQMID_MQ;
            i->enable_abr = false;
            ret++;
        } else if (streq(ldac_eqmid_str, "auto") ||
                   streq(ldac_eqmid_str, "abr")) {
            i->eqmid = LDACBT_EQMID_HQ;
            if(ldac_abr_loaded)
                i->enable_abr = true;
            ret++;
        } else {
            pa_log("ldac_eqmid parameter must be either hq, sq, mq, or auto/abr (found %s)", ldac_eqmid_str);
        }
    }

    if (ldac_fmt_str) {
        if (streq(ldac_fmt_str, "s16")) {
            i->force_pa_fmt = PA_SAMPLE_S16LE;
            ret++;
        } else if (streq(ldac_fmt_str, "s24")) {
            i->force_pa_fmt = PA_SAMPLE_S24LE;
            ret++;
        } else if (streq(ldac_fmt_str, "s32")) {
            i->force_pa_fmt = PA_SAMPLE_S32LE;
            ret++;
        } else if (streq(ldac_fmt_str, "f32")) {
            i->force_pa_fmt = PA_SAMPLE_FLOAT32LE;
            ret++;
        } else if (streq(ldac_fmt_str, "auto")) {
            i->force_pa_fmt = PA_SAMPLE_INVALID;
            ret++;
        } else {
            pa_log("ldac_fmt parameter must be either s16, s24, s32, f32 or auto (found %s)", ldac_fmt_str);
        }
    }

    return ret;
}

static size_t
pa_ldac_encode(uint32_t timestamp, void *write_buf, size_t write_buf_size, size_t *_encoded, void *read_cb_data,
               void **codec_data) {
    struct rtp_header *header;
    struct rtp_payload *payload;
    size_t nbytes;
    void *d;
    const void *p;
    size_t to_write, to_encode, ldac_enc_read;
    unsigned frame_count;
    ldac_info_t *ldac_info = *codec_data;
    pa_assert(ldac_info);
    pa_assert(ldac_info->hLdacBt);


    if (ldac_info->hLdacAbr && ldac_info->enable_abr) {
        ldac_ABR_Proc_func(ldac_info->hLdacBt, ldac_info->hLdacAbr,
                      (unsigned int) (ldac_info->tx_length / ldac_info->q_write_block_size),
                      (unsigned int) ldac_info->enable_abr);
    }


    ldac_enc_read = (pa_frame_size(&ldac_info->sample_spec) * LDACBT_ENC_LSU);

    header = write_buf;
    payload = (struct rtp_payload *) ((uint8_t *) write_buf + sizeof(*header));

    frame_count = 0;

    /* MAX pcm size for 1 LDAC packet (LDAC MQ) */
    to_encode = (ldac_info->mtu - sizeof(struct rtp_header) - sizeof(struct rtp_payload))
                / 110 * ldac_info->pcm_read_size;

    d = (uint8_t *) write_buf + sizeof(*header) + sizeof(*payload);
    to_write = write_buf_size - sizeof(*header) - sizeof(*payload);

    *_encoded = 0;
    while (PA_LIKELY(to_encode > 0 && to_write > 0 && frame_count == 0)) {
        int written;
        int encoded;
        int ldac_frame_num;
        int ret_code;
        ldac_info->read_pcm(&p, ldac_enc_read, read_cb_data);

        ret_code = ldacBT_encode_func(ldac_info->hLdacBt, (void *) p, &encoded, (uint8_t *) d, &written, &ldac_frame_num);

        ldac_info->read_buf_free(&p, read_cb_data);

        if (PA_UNLIKELY(ret_code < 0)) {
            int err;
            pa_log_error("LDAC encoding error, written:%d encoded:%d ldac_frame_num:%d", written, encoded,
                         ldac_frame_num);
            err = ldacBT_get_error_code_func(ldac_info->hLdacBt);
            pa_log_error("LDACBT_API_ERR:%d  LDACBT_HANDLE_ERR:%d  LDACBT_BLOCK_ERR:%d", LDACBT_API_ERR(err),
                         LDACBT_HANDLE_ERR(err), LDACBT_BLOCK_ERR(err));
            *_encoded = 0;
            return 0;
        }

        pa_assert_fp(encoded == (int) ldac_enc_read);
        pa_assert_fp(written <= (int) to_write);

        *_encoded += encoded;
        to_encode -= encoded;

        d = (uint8_t *) d + written;
        to_write -= written;

        frame_count += ldac_frame_num;

    }


    PA_ONCE_BEGIN
                {
                    const int v = ldacBT_get_version_func();
                    pa_log_notice("Using LDAC library: version: %x.%02x.%02x",
                                  v >> 16,
                                  (v >> 8) & 0x0ff,
                                  v & 0x0ff
                    );
                }
    PA_ONCE_END;

    /* write it to the fifo */
    memset(write_buf, 0, sizeof(*header) + sizeof(*payload));
    header->v = 2;
    header->pt = 1;
    header->sequence_number = htons(ldac_info->seq_num++);
    header->timestamp = htonl(timestamp);
    header->ssrc = htonl(1);
    payload->frame_count = frame_count;
    ldac_info->layer_specific += frame_count;

    nbytes = (uint8_t *) d - (uint8_t *) write_buf;

    ldac_info->written += nbytes - sizeof(*header) - sizeof(*payload);

    return nbytes;
}

static void
pa_ldac_config_transport(pa_sample_spec default_sample_spec, const void *configuration, size_t configuration_size,
                         pa_sample_spec *sample_spec, void **codec_data) {
    ldac_info_t *ldac_info = *codec_data;
    a2dp_ldac_t *config = (a2dp_ldac_t *) configuration;
    pa_sample_format_t fmt;
    pa_assert(ldac_info);
    pa_assert_se(configuration_size == sizeof(*config));

    ldac_info->hLdacBt = NULL;
    ldac_info->hLdacAbr = NULL;

    if (ldac_info->force_pa_fmt == PA_SAMPLE_INVALID)
        fmt = default_sample_spec.format;
    else
        fmt = ldac_info->force_pa_fmt;

    switch (fmt) {
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
            ldac_info->pcm_fmt = LDACBT_SMPL_FMT_F32;
            sample_spec->format = PA_SAMPLE_FLOAT32LE;
            break;
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_S32BE:
            ldac_info->pcm_fmt = LDACBT_SMPL_FMT_S32;
            sample_spec->format = PA_SAMPLE_S32LE;
            break;
        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
            ldac_info->pcm_fmt = LDACBT_SMPL_FMT_S24;
            sample_spec->format = PA_SAMPLE_S24LE;
            break;
        default:
            ldac_info->pcm_fmt = LDACBT_SMPL_FMT_S16;
            sample_spec->format = PA_SAMPLE_S16LE;
    }


    switch (config->frequency) {
        case LDACBT_SAMPLING_FREQ_044100:
            ldac_info->pcm_frequency = 44100U;
            sample_spec->rate = 44100U;
            break;
        case LDACBT_SAMPLING_FREQ_048000:
            ldac_info->pcm_frequency = 48000U;
            sample_spec->rate = 48000U;
            break;
        case LDACBT_SAMPLING_FREQ_088200:
            ldac_info->pcm_frequency = 88200U;
            sample_spec->rate = 88200U;
            break;
        case LDACBT_SAMPLING_FREQ_096000:
            ldac_info->pcm_frequency = 96000U;
            sample_spec->rate = 96000U;
            break;
        case LDACBT_SAMPLING_FREQ_176400:
            ldac_info->pcm_frequency = 176400U;
            sample_spec->rate = 176400U;
            break;
        case LDACBT_SAMPLING_FREQ_192000:
            ldac_info->pcm_frequency = 192000U;
            sample_spec->rate = 192000U;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (config->channel_mode) {
        case LDACBT_CHANNEL_MODE_MONO:
            ldac_info->channel_mode = LDACBT_CHANNEL_MODE_MONO;
            sample_spec->channels = 1;
            break;
        case LDACBT_CHANNEL_MODE_DUAL_CHANNEL:
            ldac_info->channel_mode = LDACBT_CHANNEL_MODE_DUAL_CHANNEL;
            sample_spec->channels = 2;
            break;
        case LDACBT_CHANNEL_MODE_STEREO:
            ldac_info->channel_mode = LDACBT_CHANNEL_MODE_STEREO;
            sample_spec->channels = 2;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (ldac_info->pcm_frequency) {
        case 44100:
        case 48000:
            ldac_info->pcm_lsu = 128;
            break;
        case 88200:
        case 96000:
            ldac_info->pcm_lsu = 256;
            break;
        case 176400:
        case 192000:
            ldac_info->pcm_lsu = 512;
            break;
        default:
            pa_assert_not_reached();
    }

    switch (ldac_info->eqmid) {
        case LDACBT_EQMID_HQ:
            ldac_info->ldac_frame_size = 330;
            break;
        case LDACBT_EQMID_SQ:
            ldac_info->ldac_frame_size = 220;
            break;
        case LDACBT_EQMID_MQ:
            ldac_info->ldac_frame_size = 110;
            break;
        default:
            pa_assert_not_reached();
    }

    ldac_info->sample_spec = *sample_spec;
    ldac_info->pcm_read_size = (ldac_info->pcm_lsu * pa_frame_size(&ldac_info->sample_spec));

};

static void pa_ldac_get_block_size(size_t write_link_mtu, size_t *write_block_size, void **codec_data) {
    ldac_info_t *ldac_info = *codec_data;
    pa_assert(ldac_info);

    ldac_info->mtu = write_link_mtu;

    ldac_info->q_write_block_size = ((write_link_mtu - sizeof(struct rtp_header) - sizeof(struct rtp_payload))
                                     / ldac_info->ldac_frame_size * ldac_info->pcm_read_size);
    *write_block_size = LDACBT_MAX_LSU * pa_frame_size(&ldac_info->sample_spec);
};


static void pa_ldac_setup_stream(void **codec_data) {
    int ret;
    ldac_info_t *ldac_info = *codec_data;
    pa_assert(ldac_info);

    ldac_info->layer_specific = 0;
    ldac_info->written = 0;
    if (ldac_info->hLdacBt)
        ldacBT_free_handle_func(ldac_info->hLdacBt);
    ldac_info->hLdacBt = ldacBT_get_handle_func();


    ret = ldacBT_init_handle_encode_func(ldac_info->hLdacBt,
                                    (int) ldac_info->mtu + AVDT_MEDIA_HDR_SIZE,
                                    ldac_info->eqmid,
                                    ldac_info->channel_mode,
                                    ldac_info->pcm_fmt,
                                    ldac_info->pcm_frequency);
    if (ret != 0) {
        pa_log_warn("Failed to init ldacBT handle");
        goto fail;
    }

    if (!ldac_abr_loaded)
        return;

    if (ldac_info->hLdacAbr)
        ldac_ABR_free_handle_func(ldac_info->hLdacAbr);
    ldac_info->hLdacAbr = ldac_ABR_get_handle_func();

    ret = ldac_ABR_Init_func(ldac_info->hLdacAbr,
                        (unsigned int) pa_bytes_to_usec(ldac_info->q_write_block_size, &ldac_info->sample_spec) / 1000);
    if (ret != 0) {
        pa_log_warn("Failed to init ldacBT_ABR handle");
        goto fail1;
    }

    ldac_ABR_set_thresholds_func(ldac_info->hLdacAbr, LDAC_ABR_THRESHOLD_CRITICAL,
                            LDAC_ABR_THRESHOLD_DANGEROUSTREND, LDAC_ABR_THRESHOLD_SAFETY_FOR_HQSQ);
    return;

fail:
    ldacBT_free_handle_func(ldac_info->hLdacBt);
    ldac_info->hLdacBt = NULL;
    if (!ldac_abr_loaded)
        return;
fail1:
    ldac_ABR_free_handle_func(ldac_info->hLdacAbr);
    ldac_info->hLdacAbr = NULL;
    ldac_info->enable_abr = false;
};

static void pa_ldac_set_tx_length(size_t len, void **codec_data) {
    ldac_info_t *ldac_info = *codec_data;
    pa_assert(ldac_info);
    ldac_info->tx_length = len;
};

static void pa_ldac_free(void **codec_data) {
    ldac_info_t *ldac_info = *codec_data;
    if (!ldac_info)
        return;

    if (ldac_info->hLdacBt)
        ldacBT_free_handle_func(ldac_info->hLdacBt);

    if (ldac_info->hLdacAbr && ldac_abr_loaded)
        ldac_ABR_free_handle_func(ldac_info->hLdacAbr);

    pa_xfree(ldac_info);
    *codec_data = NULL;

};

static size_t pa_ldac_get_capabilities(void **_capabilities) {
    a2dp_ldac_t *capabilities = pa_xmalloc0(sizeof(a2dp_ldac_t));

    capabilities->info.vendor_id = LDAC_VENDOR_ID;
    capabilities->info.codec_id = LDAC_CODEC_ID;
    capabilities->frequency = LDACBT_SAMPLING_FREQ_044100 | LDACBT_SAMPLING_FREQ_048000 |
                              LDACBT_SAMPLING_FREQ_088200 | LDACBT_SAMPLING_FREQ_096000;
    capabilities->channel_mode = LDACBT_CHANNEL_MODE_MONO | LDACBT_CHANNEL_MODE_DUAL_CHANNEL |
                                 LDACBT_CHANNEL_MODE_STEREO;
    *_capabilities = capabilities;

    return sizeof(*capabilities);
};

static size_t
pa_ldac_select_configuration(const pa_sample_spec default_sample_spec, const uint8_t *supported_capabilities,
                             const size_t capabilities_size, void **configuration) {
    a2dp_ldac_t *cap = (a2dp_ldac_t *) supported_capabilities;
    a2dp_ldac_t *config = pa_xmalloc0(sizeof(a2dp_ldac_t));
    pa_a2dp_freq_cap_t ldac_freq_cap, ldac_freq_table[] = {
            {44100U,  LDACBT_SAMPLING_FREQ_044100},
            {48000U,  LDACBT_SAMPLING_FREQ_048000},
            {88200U,  LDACBT_SAMPLING_FREQ_088200},
            {96000U,  LDACBT_SAMPLING_FREQ_096000}
    };

    if (capabilities_size != sizeof(a2dp_ldac_t))
        return 0;

    config->info.vendor_id = LDAC_VENDOR_ID;
    config->info.codec_id = LDAC_CODEC_ID;

    if (!pa_a2dp_select_cap_frequency(cap->frequency, default_sample_spec, ldac_freq_table,
                                      PA_ELEMENTSOF(ldac_freq_table), &ldac_freq_cap))
        return 0;

    config->frequency = (uint8_t) ldac_freq_cap.cap;

    if (default_sample_spec.channels <= 1) {
        if (cap->channel_mode & LDACBT_CHANNEL_MODE_MONO)
            config->channel_mode = LDACBT_CHANNEL_MODE_MONO;
        else if (cap->channel_mode & LDACBT_CHANNEL_MODE_STEREO)
            config->channel_mode = LDACBT_CHANNEL_MODE_STEREO;
        else if (cap->channel_mode & LDACBT_CHANNEL_MODE_DUAL_CHANNEL)
            config->channel_mode = LDACBT_CHANNEL_MODE_DUAL_CHANNEL;
        else {
            pa_log_error("No supported channel modes");
            return 0;
        }
    }

    if (default_sample_spec.channels >= 2) {
        if (cap->channel_mode & LDACBT_CHANNEL_MODE_STEREO)
            config->channel_mode = LDACBT_CHANNEL_MODE_STEREO;
        else if (cap->channel_mode & LDACBT_CHANNEL_MODE_DUAL_CHANNEL)
            config->channel_mode = LDACBT_CHANNEL_MODE_DUAL_CHANNEL;
        else if (cap->channel_mode & LDACBT_CHANNEL_MODE_MONO)
            config->channel_mode = LDACBT_CHANNEL_MODE_MONO;
        else {
            pa_log_error("No supported channel modes");
            return 0;
        }
    }
    *configuration = config;
    return sizeof(*config);
};

static void pa_ldac_free_capabilities(void **capabilities) {
    if (!capabilities || !*capabilities)
        return;
    pa_xfree(*capabilities);
    *capabilities = NULL;
}

static bool pa_ldac_set_configuration(const uint8_t *selected_configuration, const size_t configuration_size) {
    a2dp_ldac_t *c = (a2dp_ldac_t *) selected_configuration;

    if (configuration_size != sizeof(a2dp_ldac_t)) {
        pa_log_error("LDAC configuration array of invalid size");
        return false;
    }

    switch (c->frequency) {
        case LDACBT_SAMPLING_FREQ_044100:
        case LDACBT_SAMPLING_FREQ_048000:
        case LDACBT_SAMPLING_FREQ_088200:
        case LDACBT_SAMPLING_FREQ_096000:
        case LDACBT_SAMPLING_FREQ_176400:
        case LDACBT_SAMPLING_FREQ_192000:
            break;
        default:
            pa_log_error("Invalid sampling frequency in LDAC configuration");
            return false;
    }

    switch (c->channel_mode) {
        case LDACBT_CHANNEL_MODE_STEREO:
        case LDACBT_CHANNEL_MODE_DUAL_CHANNEL:
        case LDACBT_CHANNEL_MODE_MONO:
            break;
        default:
            pa_log_error("Invalid channel mode in LDAC Configuration");
            return false;
    }

    return true;
};


static pa_a2dp_source_t pa_ldac_source = {
        .encoder_load = pa_ldac_encoder_load,
        .init = pa_ldac_encoder_init,
        .update_user_config = pa_ldac_update_user_config,
        .encode = pa_ldac_encode,
        .config_transport=pa_ldac_config_transport,
        .get_block_size=pa_ldac_get_block_size,
        .setup_stream = pa_ldac_setup_stream,
        .set_tx_length = pa_ldac_set_tx_length,
        .decrease_quality = NULL,
        .free = pa_ldac_free
};

const pa_a2dp_codec_t pa_a2dp_ldac = {
        .name = "LDAC",
        .codec = A2DP_CODEC_VENDOR,
        .vendor_codec = &((a2dp_vendor_codec_t) {
                .vendor_id = LDAC_VENDOR_ID,
                .codec_id = LDAC_CODEC_ID
        }),
        .a2dp_sink = NULL,
        .a2dp_source = &pa_ldac_source,
        .get_capabilities = pa_ldac_get_capabilities,
        .select_configuration = pa_ldac_select_configuration,
        .free_capabilities = pa_ldac_free_capabilities,
        .free_configuration = pa_ldac_free_capabilities,
        .set_configuration = pa_ldac_set_configuration
};