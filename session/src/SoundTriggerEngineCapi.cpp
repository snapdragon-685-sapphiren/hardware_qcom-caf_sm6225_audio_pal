/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "PAL: SoundTriggerEngineCapi"

#include "SoundTriggerEngineCapi.h"

#include <dlfcn.h>

#include "StreamSoundTrigger.h"
#include "Stream.h"

void SoundTriggerEngineCapi::BufferThreadLoop(
    SoundTriggerEngineCapi *capi_engine)
{
    StreamSoundTrigger *s = nullptr;
    int32_t status = 0;

    PAL_DBG(LOG_TAG, "Enter");
    if (!capi_engine) {
        PAL_ERR(LOG_TAG, "Invalid sound trigger capi engine");
        return;
    }

    std::unique_lock<std::mutex> lck(capi_engine->event_mutex_);
    while (!capi_engine->exit_thread_) {
        PAL_VERBOSE(LOG_TAG, "waiting on cond, processing started  = %d",
                    capi_engine->processing_started_);
        // Wait for keyword buffer data from DSP
        if (!capi_engine->processing_started_)
            capi_engine->cv_.wait(lck);
        PAL_VERBOSE(LOG_TAG, "done waiting on cond, exit buffering = %d",
                    capi_engine->exit_buffering_);

        if (capi_engine->exit_thread_) {
            break;
        }

        /*
         * If 1st stage buffering overflows before 2nd stage starts processing,
         * the below functions need to be called to reset the 1st stage session
         * for the next detection. We might be able to check states of the engine
         * to avoid this buffering flag.
         */
        if (capi_engine->exit_buffering_) {
            continue;  // skip over processing if we want to exit already
        }

        if (capi_engine->processing_started_) {
            s = dynamic_cast<StreamSoundTrigger *>(capi_engine->stream_handle_);
            if (capi_engine->detection_type_ ==
                ST_SM_TYPE_KEYWORD_DETECTION) {
                status = capi_engine->StartKeywordDetection();
                if (status || !capi_engine->keyword_detected_)
                    s->SetEngineDetectionState(KEYWORD_DETECTION_REJECT);
                else
                    s->SetEngineDetectionState(KEYWORD_DETECTION_SUCCESS);
            } else if (capi_engine->detection_type_ ==
                ST_SM_TYPE_USER_VERIFICATION) {
                status = capi_engine->StartUserVerification();
                if (status || !capi_engine->keyword_detected_)
                    s->SetEngineDetectionState(USER_VERIFICATION_REJECT);
                else
                    s->SetEngineDetectionState(USER_VERIFICATION_SUCCESS);
            }
            capi_engine->keyword_detected_ = false;
            capi_engine->processing_started_ = false;
        }
    }
    PAL_DBG(LOG_TAG, "Exit");
}

int32_t SoundTriggerEngineCapi::StartKeywordDetection()
{
    int32_t status = 0;
    char *process_input_buff = nullptr;
    capi_v2_err_t rc = CAPI_V2_EOK;
    capi_v2_stream_data_t *stream_input = nullptr;
    sva_result_t *result_cfg_ptr = nullptr;
    int32_t read_size = 0;
    size_t start_idx = 0;
    size_t end_idx = 0;
    capi_v2_buf_t capi_result;
    bool buffer_advanced = false;

    PAL_DBG(LOG_TAG, "Enter");
    if (!reader_) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid ring buffer reader");
        goto exit;
    }

    reader_->getIndices(&buffer_start_, &buffer_end_);
    if (buffer_start_ >= buffer_end_) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid keyword indices");
        goto exit;
    }

    // calculate start and end index including tolerance
    if (buffer_start_ > UsToBytes(kw_start_tolerance_)) {
        buffer_start_ -= UsToBytes(kw_start_tolerance_);
    } else {
        buffer_start_ = 0;
    }

    buffer_end_ += UsToBytes(kw_end_tolerance_);
    PAL_DBG(LOG_TAG, "buffer_start_: %u, buffer_end_: %u",
        buffer_start_, buffer_end_);

    memset(&capi_result, 0, sizeof(capi_result));
    process_input_buff = (char*)calloc(1, buffer_size_);
    if (!process_input_buff) {
        status = -ENOMEM;
        PAL_ERR(LOG_TAG, "failed to allocate process input buff, status %d",
                status);
        goto exit;
    }

    stream_input = (capi_v2_stream_data_t *)
                   calloc(1, sizeof(capi_v2_stream_data_t));
    if (!stream_input) {
        status = -ENOMEM;
        PAL_ERR(LOG_TAG, "failed to allocate stream input, status %d", status);
        goto exit;
    }

    stream_input->buf_ptr = (capi_v2_buf_t*)calloc(1, sizeof(capi_v2_buf_t));
    if (!stream_input->buf_ptr) {
        status = -ENOMEM;
        PAL_ERR(LOG_TAG, "failed to allocate stream_input->buf_ptr, status %d",
                status);
        goto exit;
    }

    result_cfg_ptr = (sva_result_t*)calloc(1, sizeof(sva_result_t));
    if (!result_cfg_ptr) {
        status = -ENOMEM;
        PAL_ERR(LOG_TAG, "failed to allocate result cfg ptr status %d", status);
        goto exit;
    }

    bytes_processed_ = 0;

    while (!exit_buffering_ &&
        (bytes_processed_ < buffer_end_ - buffer_start_)) {
        /* Original code had some time of wait will need to revisit*/
        /* need to take into consideration the start and end buffer*/

        /* advance the offset to ensure we are reading at the right place */
        if (!buffer_advanced && buffer_start_ > 0) {
            if (reader_->advanceReadOffset(buffer_start_)) {
                buffer_advanced = true;
            } else {
                continue;
            }
        }

        if (reader_->getUnreadSize() < buffer_size_)
            continue;

        read_size = reader_->read((void*)process_input_buff, buffer_size_);
        if (read_size == 0)
            continue;

        PAL_INFO(LOG_TAG, "Processed: %u, start: %u, end: %u",
                 bytes_processed_, buffer_start_, buffer_end_);
        stream_input->bufs_num = 1;
        stream_input->buf_ptr->max_data_len = buffer_size_;
        stream_input->buf_ptr->actual_data_len = read_size;
        stream_input->buf_ptr->data_ptr = (int8_t *)process_input_buff;

        PAL_VERBOSE(LOG_TAG, "Calling Capi Process");

        rc = capi_handle_->vtbl_ptr->process(capi_handle_,
            &stream_input, nullptr);

        if (CAPI_V2_EFAILED == rc) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "capi process failed, status %d", status);
            goto exit;
        }

        bytes_processed_ += read_size;

        capi_result.data_ptr = (int8_t*)result_cfg_ptr;
        capi_result.actual_data_len = sizeof(sva_result_t);
        capi_result.max_data_len = sizeof(sva_result_t);

        PAL_VERBOSE(LOG_TAG, "Calling Capi get param for status");

        rc = capi_handle_->vtbl_ptr->get_param(capi_handle_,
            SVA_ID_RESULT, nullptr, &capi_result);

        if (CAPI_V2_EFAILED == rc) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "capi get param failed, status %d", status);
            goto exit;
        }

        if (result_cfg_ptr->is_detected) {
            exit_buffering_ = true;
            keyword_detected_ = true;
            start_idx = (result_cfg_ptr->start_position * CNN_FRAME_SIZE) +
                buffer_start_;
            end_idx = (result_cfg_ptr->end_position * CNN_FRAME_SIZE) +
                buffer_start_;
            PAL_INFO(LOG_TAG, "KW Second Stage Detected, start index %zu, end index %zu",
                start_idx, end_idx);
        }
    }

exit:
    if (reader_)
        reader_->updateState(READER_DISABLED);

    if (process_input_buff)
        free(process_input_buff);
    if (stream_input) {
        if (stream_input->buf_ptr)
            free(stream_input->buf_ptr);
        free(stream_input);
    }
    if (result_cfg_ptr)
        free(result_cfg_ptr);

    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SoundTriggerEngineCapi::StartUserVerification()
{
    int32_t status = 0;
    char *process_input_buff = nullptr;
    capi_v2_err_t rc = CAPI_V2_EOK;
    capi_v2_stream_data_t *stream_input = nullptr;
    capi_v2_buf_t capi_uv_ptr;
    voiceprint2_result_t *result_cfg_ptr = nullptr;
    voiceprint2_sva_uv_score_t *uv_cfg_ptr = nullptr;
    int32_t read_size = 0;
    capi_v2_buf_t capi_result;
    bool buffer_advanced = false;
    StreamSoundTrigger *str = nullptr;
    struct detection_event_info *info = nullptr;

    PAL_DBG(LOG_TAG, "Enter");
    if (!reader_) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid ring buffer reader");
        goto exit;
    }

    str = dynamic_cast<StreamSoundTrigger *>(stream_handle_);
    info = str->GetDetectionEventInfo();
    if (!info) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Failed to get detection event info");
        goto exit;;
    }
    confidence_score_ = info->confidence_levels[1];

    reader_->getIndices(&buffer_start_, &buffer_end_);
    if (buffer_start_ >= buffer_end_) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid keyword indices");
        goto exit;
    }

    // calculate start and end index including tolerance
    if (buffer_start_ > UsToBytes(kw_start_tolerance_)) {
        buffer_start_ -= UsToBytes(kw_start_tolerance_);
    } else {
        buffer_start_ = 0;
    }

    buffer_end_ += UsToBytes(kw_end_tolerance_);
    buffer_size_ = buffer_end_ - buffer_start_;

    memset(&capi_uv_ptr, 0, sizeof(capi_uv_ptr));
    memset(&capi_result, 0, sizeof(capi_result));

    process_input_buff = (char*)calloc(1, buffer_size_);
    if (!process_input_buff) {
        PAL_ERR(LOG_TAG, "failed to allocate process input buff");
        status = -ENOMEM;
        goto exit;
    }

    stream_input = (capi_v2_stream_data_t *)
                   calloc(1, sizeof(capi_v2_stream_data_t));
    if (!stream_input) {
        PAL_ERR(LOG_TAG, "failed to allocate stream input");
        status = -ENOMEM;
        goto exit;
    }

    stream_input->buf_ptr = (capi_v2_buf_t*)calloc(1, sizeof(capi_v2_buf_t));
    if (!stream_input->buf_ptr) {
        PAL_ERR(LOG_TAG, "failed to allocate buf ptr");
        status = -ENOMEM;
        goto exit;
    }

    result_cfg_ptr = (voiceprint2_result_t*)
                     calloc(1, sizeof(voiceprint2_result_t));
    if (!result_cfg_ptr) {
        PAL_ERR(LOG_TAG, "failed to allocate result cfg ptr");
        status = -ENOMEM;
        goto exit;
    }

    uv_cfg_ptr = (voiceprint2_sva_uv_score_t *)
                 calloc(1, sizeof(voiceprint2_sva_uv_score_t));
    if (!uv_cfg_ptr) {
        PAL_ERR(LOG_TAG, "failed to allocate uv cfg ptr");
        status = -ENOMEM;
        goto exit;
    }

    uv_cfg_ptr->sva_uv_confidence_score = confidence_score_;
    capi_uv_ptr.data_ptr = (int8_t *)uv_cfg_ptr;
    capi_uv_ptr.actual_data_len = sizeof(voiceprint2_sva_uv_score_t);
    capi_uv_ptr.max_data_len = sizeof(voiceprint2_sva_uv_score_t);

    PAL_VERBOSE(LOG_TAG, "Issuing capi_set_param for param %d",
                VOICEPRINT2_ID_SVA_UV_SCORE);
    rc = capi_handle_->vtbl_ptr->set_param(capi_handle_,
        VOICEPRINT2_ID_SVA_UV_SCORE, nullptr, &capi_uv_ptr);
    if (CAPI_V2_EOK != rc) {
        PAL_ERR(LOG_TAG, "set param VOICEPRINT2_ID_SVA_UV_SCORE failed with %d",
                rc);
        status = -EINVAL;
        goto exit;
    }

    if (kw_end_timestamp_ > 0)
        buffer_end_ = UsToBytes(kw_end_timestamp_);

    if (kw_start_timestamp_ > 0)
        buffer_start_ = UsToBytes(kw_start_timestamp_);

    bytes_processed_ = 0;

    while (!exit_buffering_ &&
        (bytes_processed_ < buffer_end_ - buffer_start_)) {
        /* Original code had some time of wait will need to revisit*/
        /* need to take into consideration the start and end buffer*/

        /* advance the offset to ensure we are reading at the right place */
        if (!buffer_advanced && buffer_start_ > 0) {
            if (reader_->advanceReadOffset(buffer_start_)) {
                buffer_advanced = true;
            } else {
                continue;
            }
        }

        if (reader_->getUnreadSize() < buffer_size_)
            continue;

        read_size = reader_->read((void*)process_input_buff, buffer_size_);
        if (read_size == 0)
            continue;
        PAL_INFO(LOG_TAG, "Processed: %u, start: %u, end: %u",
                 bytes_processed_, buffer_start_, buffer_end_);
        stream_input->bufs_num = 1;
        stream_input->buf_ptr->max_data_len = buffer_size_;
        stream_input->buf_ptr->actual_data_len = read_size;
        stream_input->buf_ptr->data_ptr = (int8_t *)process_input_buff;

        PAL_VERBOSE(LOG_TAG, "Calling Capi Process\n");

        rc = capi_handle_->vtbl_ptr->process(capi_handle_,
            &stream_input, nullptr);

        if (CAPI_V2_EFAILED == rc) {
            PAL_ERR(LOG_TAG, "capi process failed\n");
            status = -EINVAL;
            goto exit;
        }

        bytes_processed_ += read_size;

        capi_result.data_ptr = (int8_t*)result_cfg_ptr;
        capi_result.actual_data_len = sizeof(voiceprint2_result_t);
        capi_result.max_data_len = sizeof(voiceprint2_result_t);

        PAL_VERBOSE(LOG_TAG, "Calling Capi get param for result\n");

        rc = capi_handle_->vtbl_ptr->get_param(capi_handle_,
            VOICEPRINT2_ID_RESULT, nullptr, &capi_result);

        if (CAPI_V2_EFAILED == rc) {
            PAL_ERR(LOG_TAG, "capi get param failed\n");
            status = -EINVAL;
            goto exit;
        }

        if (result_cfg_ptr->is_detected) {
            exit_buffering_ = true;
            keyword_detected_ = true;
            PAL_INFO(LOG_TAG, "KW Second Stage Detected");
        }
    }

exit:
    if (reader_)
        reader_->updateState(READER_DISABLED);

    if (process_input_buff)
        free(process_input_buff);
    if (stream_input) {
        if (stream_input->buf_ptr)
            free(stream_input->buf_ptr);
        free(stream_input);
    }
    if (result_cfg_ptr)
        free(result_cfg_ptr);
    if (uv_cfg_ptr)
        free(uv_cfg_ptr);

    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

SoundTriggerEngineCapi::SoundTriggerEngineCapi(
    Stream *s,
    uint32_t id,
    listen_model_indicator_enum type)
{
    int32_t status = 0;

    PAL_DBG(LOG_TAG, "Enter");
    engine_id_ = id;
    engine_type_ = type;
    processing_started_ = false;
    keyword_detected_ = false;
    sm_data_ = nullptr;
    exit_thread_ = false;
    exit_buffering_ = false;
    buffer_size_ = CNN_BUFFER_SIZE;  // 480ms of 16k 16bit mono worth;
    kw_start_timestamp_ = 0;
    kw_end_timestamp_ = 0;
    buffer_start_ = 0;
    buffer_end_ = 0;
    bytes_processed_ = 0;
    reader_ = nullptr;
    buffer_ = nullptr;
    stream_handle_ = s;

    StreamSoundTrigger *st_str = dynamic_cast<StreamSoundTrigger *>(s);
    status = st_str->GetEngineConfig(sample_rate_,
        bit_width_, channels_, type);
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to get engine config");
        throw std::runtime_error("Failed to get engine config");
    }

    status = st_str->GetSecondStageConfig(detection_type_,
        lib_name_, type);
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to get ss engine config");
        throw std::runtime_error("Failed to get ss engine config");
    }

    kw_start_tolerance_ = st_str->GetKwStartTolerance();
    kw_end_tolerance_ = st_str->GetKwEndTolerance();

    // TODO: ST_SM_TYPE_CUSTOM_DETECTION
    if (detection_type_ == ST_SM_TYPE_KEYWORD_DETECTION) {
        capi_handle_ = (capi_v2_t *)calloc(1,
            sizeof(capi_v2_t) + sizeof(char *));
    } else if (detection_type_ == ST_SM_TYPE_USER_VERIFICATION) {
        capi_handle_ = (capi_v2_t *)calloc(1,
            sizeof(capi_v2_t) + (3 * sizeof(char *)));
    }

    if (!capi_handle_) {
        status = -ENOMEM;
        PAL_ERR(LOG_TAG, "failed to allocate capi handle = %d", status);
        /* handle here */
        goto err_exit;
    }

    capi_lib_handle_ = dlopen(lib_name_.c_str(), RTLD_NOW);
    if (!capi_lib_handle_) {
        status = -ENOMEM;
        PAL_ERR(LOG_TAG,  "failed to open capi so = %d", status);
        /* handle here */
        goto err_exit;
    }

    dlerror();

    capi_init_ = (capi_v2_init_f)dlsym(capi_lib_handle_, "capi_v2_init");

    if (!capi_init_) {
        PAL_ERR(LOG_TAG,  "failed to map capi init function = %d", status);
        /* handle here */
        goto err_exit;
    }

    return;
err_exit:
    PAL_ERR(LOG_TAG, "constructor exit status = %d", status);
}

SoundTriggerEngineCapi::~SoundTriggerEngineCapi()
{
    PAL_DBG(LOG_TAG, "Enter");
    if (buffer_) {
        delete buffer_;
    }
    if (reader_) {
        delete reader_;
    }
    if (capi_lib_handle_) {
        dlclose(capi_lib_handle_);
        capi_lib_handle_ = nullptr;
    }
    if (capi_handle_) {
        capi_handle_->vtbl_ptr = nullptr;
        free(capi_handle_);
        capi_handle_ = nullptr;
    }
    PAL_DBG(LOG_TAG, "Exit");
}

int32_t SoundTriggerEngineCapi::StartSoundEngine()
{
    int32_t status = 0;
    processing_started_ = false;
    exit_thread_ = false;
    exit_buffering_ = false;
    capi_v2_err_t rc = CAPI_V2_EOK;
    capi_v2_buf_t capi_buf;

    PAL_DBG(LOG_TAG, "Enter");
    buffer_thread_handler_ =
        std::thread(SoundTriggerEngineCapi::BufferThreadLoop, this);

    if (!buffer_thread_handler_.joinable()) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "failed to create buffer thread = %d", status);
        return status;
    }

    if (detection_type_ == ST_SM_TYPE_KEYWORD_DETECTION) {
        sva_threshold_config_t *threshold_cfg = nullptr;
        threshold_cfg = (sva_threshold_config_t*)
                        calloc(1, sizeof(sva_threshold_config_t));
        if (!threshold_cfg) {
            status = -ENOMEM;
            PAL_ERR(LOG_TAG, "threshold cfg calloc failed, status %d", status);
            return status;
        }
        capi_buf.data_ptr = (int8_t*) threshold_cfg;
        capi_buf.actual_data_len = sizeof(sva_threshold_config_t);
        capi_buf.max_data_len = sizeof(sva_threshold_config_t);
        threshold_cfg->smm_threshold = confidence_threshold_;
        PAL_VERBOSE(LOG_TAG, "Keyword detection (CNN) confidence level = %d",
            confidence_threshold_);

        status = capi_handle_->vtbl_ptr->set_param(capi_handle_,
            SVA_ID_THRESHOLD_CONFIG, nullptr, &capi_buf);

        if (CAPI_V2_EOK != status) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "set param SVA_ID_THRESHOLD_CONFIG failed with %d",
                    status);
            if (threshold_cfg)
                free(threshold_cfg);
            return status;
        }

        PAL_VERBOSE(LOG_TAG, "Issuing capi_set_param for param %d",
                    SVA_ID_REINIT_ALL);
        status = capi_handle_->vtbl_ptr->set_param(capi_handle_,
            SVA_ID_REINIT_ALL, nullptr, nullptr);

        if (CAPI_V2_EOK != status) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "set param SVA_ID_REINIT_ALL failed, status = %d",
                    status);
            if (threshold_cfg)
                free(threshold_cfg);
            return status;
        }
    } else if (detection_type_ == ST_SM_TYPE_USER_VERIFICATION) {
        voiceprint2_threshold_config_t *threshold_cfg = nullptr;
        rc = capi_handle_->vtbl_ptr->set_param(capi_handle_,
            VOICEPRINT2_ID_REINIT, nullptr, nullptr);
        if (CAPI_V2_EOK != rc) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "set_param VOICEPRINT2_ID_REINIT failed, status = %d",
                    status);
            return status;
        }

        threshold_cfg = (voiceprint2_threshold_config_t *)
            calloc(1, sizeof(voiceprint2_threshold_config_t));
        if (!threshold_cfg) {
            PAL_ERR(LOG_TAG, "failed to allocate threshold cfg");
            status = -ENOMEM;
            return status;
        }

        capi_buf.data_ptr = (int8_t *)threshold_cfg;
        capi_buf.actual_data_len = sizeof(voiceprint2_threshold_config_t);
        capi_buf.max_data_len = sizeof(voiceprint2_threshold_config_t);
        threshold_cfg->user_verification_threshold = confidence_threshold_;
        PAL_VERBOSE(LOG_TAG, "Keyword detection (VOP) confidence level = %d",
                    confidence_threshold_);

        rc = capi_handle_->vtbl_ptr->set_param(capi_handle_,
            VOICEPRINT2_ID_THRESHOLD_CONFIG, nullptr, &capi_buf);

        if (CAPI_V2_EOK != rc) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "set param %d failed with %d",
                    VOICEPRINT2_ID_THRESHOLD_CONFIG, rc);
            if (threshold_cfg)
                free(threshold_cfg);
            return status;
        }
    }

    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SoundTriggerEngineCapi::StopSoundEngine()
{
    int32_t status = 0;

    PAL_DBG(LOG_TAG, "Enter");
    {
        processing_started_ = false;
        std::lock_guard<std::mutex> lck(event_mutex_);
        exit_thread_ = true;
        exit_buffering_ = true;

        cv_.notify_one();
    }
    if (buffer_thread_handler_.joinable()) {
        buffer_thread_handler_.join();
    }
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SoundTriggerEngineCapi::LoadSoundModel(Stream *s __unused,
    uint8_t *data, uint32_t data_size)
{
    int32_t status = 0;
    capi_v2_err_t rc = CAPI_V2_EOK;
    capi_v2_proplist_t init_set_proplist;
    capi_v2_prop_t sm_prop_ptr;

    PAL_DBG(LOG_TAG, "Enter");
    std::lock_guard<std::mutex> lck(mutex_);
    if (!data) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid sound model data, status %d", status);
        goto exit;
    }

    sm_data_ = data;
    sm_data_size_ = data_size;

    sm_prop_ptr.id = CAPI_V2_CUSTOM_INIT_DATA;
    sm_prop_ptr.payload.data_ptr = (int8_t *)sm_data_;
    sm_prop_ptr.payload.actual_data_len = sm_data_size_;
    sm_prop_ptr.payload.max_data_len = sm_data_size_;
    init_set_proplist.props_num = 1;
    init_set_proplist.prop_ptr = &sm_prop_ptr;

    PAL_VERBOSE(LOG_TAG, "Issuing capi_init");
    rc = capi_init_(capi_handle_, &init_set_proplist);

    if (rc != CAPI_V2_EOK) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "capi_init status is %d, exiting, status %d",
                rc, status);
        goto exit;
    }

    if (nullptr == capi_handle_) {
        PAL_ERR(LOG_TAG, "capi_handle is nullptr, exiting");
        status = -EINVAL;
        goto exit;
    }

    if (!capi_handle_->vtbl_ptr) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "capi_handle->vtbl_ptr is nullptr, exiting, status %d",
                status);
        goto exit;
    }

exit:
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SoundTriggerEngineCapi::UnloadSoundModel(Stream *s __unused)
{
    int32_t status = 0;

    PAL_DBG(LOG_TAG, "Enter, Issuing capi_end");
    status = capi_handle_->vtbl_ptr->end(capi_handle_);
    if (status != CAPI_V2_EOK) {
        PAL_ERR(LOG_TAG, "Capi end function failed, status = %d",
            status);
        status = -EINVAL;
    }

    return status;
}

int32_t SoundTriggerEngineCapi::StartRecognition(Stream *s __unused)
{
    int32_t status = 0;

    PAL_DBG(LOG_TAG, "Enter");
    std::lock_guard<std::mutex> lck(mutex_);
    status = StartSoundEngine();
    if (0 != status) {
        PAL_ERR(LOG_TAG, "Failed to start sound engine, status = %d", status);
        goto exit;
    }

exit:
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SoundTriggerEngineCapi::StopBuffering(Stream *s __unused)
{
    int32_t status = 0;

    PAL_DBG(LOG_TAG, "Enter");
    std::lock_guard<std::mutex> lck(mutex_);
    processing_started_ = false;
    exit_buffering_ = true;
    if (reader_) {
        reader_->reset();
    } else {
        status = -EINVAL;
        goto exit;
    }

exit:
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SoundTriggerEngineCapi::StopRecognition(Stream *s __unused)
{
    int32_t status = 0;

    PAL_DBG(LOG_TAG, "Enter");
    std::lock_guard<std::mutex> lck(mutex_);
    status = StopSoundEngine();
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to stop sound engine, status = %d", status);
        goto exit;
    }

    if (reader_) {
        reader_->reset();
    } else {
        status = -EINVAL;
        goto exit;
    }

exit:
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SoundTriggerEngineCapi::UpdateConfLevels(
    Stream *s __unused,
    struct pal_st_recognition_config *config __unused,
    uint8_t *conf_levels,
    uint32_t num_conf_levels)
{
    int32_t status = 0;

    PAL_DBG(LOG_TAG, "Enter");
    if (!conf_levels || !num_conf_levels) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid config, status %d", status);
        return status;
    }

    std::lock_guard<std::mutex> lck(mutex_);
    confidence_threshold_ = *conf_levels;
    PAL_VERBOSE(LOG_TAG, "confidence threshold: %d", confidence_threshold_);

    return status;
}

void SoundTriggerEngineCapi::SetDetected(bool detected)
{
    PAL_DBG(LOG_TAG, "SetDetected %d", detected);
    std::lock_guard<std::mutex> lck(event_mutex_);
    if (detected != processing_started_) {
        processing_started_ = detected;
        exit_buffering_ = !processing_started_;
        PAL_INFO(LOG_TAG, "setting processing started %d", detected);
        cv_.notify_one();
    } else {
        PAL_VERBOSE(LOG_TAG, "processing started unchanged");
    }
}