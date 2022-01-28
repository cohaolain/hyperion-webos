#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <dlfcn.h>
#include <libyuv.h>
#include "unicapture.h"
#include "log.h"

#define DLSYM_ERROR_CHECK()                                         \
    if ((error = dlerror()) != NULL)  {                             \
        ERR("Error! dlsym failed, msg: %s", error);                 \
        return -2;                                                  \
    }

static uint64_t getticks_us() {
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec * 1000000 + tp.tv_nsec / 1000;
}

typedef struct _converter {
    // Temporary conversion memory buffers
    uint8_t* buffers[4];
} converter_t;


void converter_init(converter_t* this) {
    for (int i = 0; i < 4; i++) {
        this->buffers[i] = NULL;
    }
}

int converter_release(converter_t* converter) {
    for (int i = 0; i < 4; i++) {
        if (converter->buffers[i] != NULL) {
            free(converter->buffers[i]);
        }
    }
}

/**
 * Perform frame conversion - all temporary buffers are stored in converter_t
 * state variable - a single converter instance may be reused multiple times,
 * however output frame shall not be used after converter_release, or when
 * another converter_run call has been issued.
 */
int converter_run(converter_t* this, frame_info_t* input, frame_info_t* output, pixel_format_t target_format) {
    if (target_format == PIXFMT_ARGB) {
        output->width = input->width;
        output->height = input->height;

        this->buffers[0] = realloc(this->buffers[0], output->width * output->height * 4);

        output->planes[0].buffer = this->buffers[0];
        output->planes[0].stride = output->width * 4;

        if (input->pixel_format == PIXFMT_RGB) {
            RGB24ToARGB(
                input->planes[0].buffer,
                input->planes[0].stride,
                output->planes[0].buffer,
                output->planes[0].stride,
                output->width,
                output->height
            );
        } else if (input->pixel_format == PIXFMT_ABGR) {
            ABGRToARGB(
                input->planes[0].buffer,
                input->planes[0].stride,
                output->planes[0].buffer,
                output->planes[0].stride,
                output->width,
                output->height
            );
        } else if (input->pixel_format == PIXFMT_YUV420_SEMI_PLANAR) {
            NV21ToARGB(
                input->planes[0].buffer,
                input->planes[0].stride,
                input->planes[1].buffer,
                input->planes[1].stride,
                output->planes[0].buffer,
                output->planes[0].stride,
                output->width,
                output->height
            );
        } else if (input->pixel_format == PIXFMT_YUV422_SEMI_PLANAR) {
            this->buffers[1] = realloc(this->buffers[1], input->width / 2 * input->height);
            this->buffers[2] = realloc(this->buffers[2], input->width / 2 * input->height);
            SplitUVPlane(
                input->planes[1].buffer,
                input->planes[1].stride,
                this->buffers[1],
                input->width/2,
                this->buffers[2],
                input->width/2,
                input->width/2,
                input->height
            );
            I422ToARGB(
                input->planes[0].buffer,
                input->planes[0].stride,
                this->buffers[2],
                input->width / 2,
                this->buffers[1],
                input->width / 2,
                output->planes[0].buffer,
                output->planes[0].stride,
                output->width,
                output->height
            );
        } else {
            return -2;
        }

        output->pixel_format = PIXFMT_ARGB;
        return 0;
    } else {
        // Only support ARGB for now...
        return -1;
    }
}

int unicapture_init_backend(cap_backend_config_t* config, capture_backend_t* backend, char* name) {
    char* error;
    void* handle = dlopen(name, RTLD_LAZY);

    DBG("%s: loading...", name);

    if (handle == NULL) {
        WARN("Unable to load %s: %s", name, dlerror());
        return -1;
    }

    dlerror();

    backend->init = dlsym(handle, "capture_init"); DLSYM_ERROR_CHECK();
    backend->cleanup = dlsym(handle, "capture_cleanup"); DLSYM_ERROR_CHECK();

    backend->start = dlsym(handle, "capture_start"); DLSYM_ERROR_CHECK();
    backend->terminate = dlsym(handle, "capture_terminate"); DLSYM_ERROR_CHECK();

    backend->acquire_frame = dlsym(handle, "capture_acquire_frame"); DLSYM_ERROR_CHECK();
    backend->release_frame = dlsym(handle, "capture_release_frame"); DLSYM_ERROR_CHECK();

    backend->wait = dlsym(handle, "capture_wait"); DLSYM_ERROR_CHECK();

    DBG("%s: loaded, initializing...", name);

    int ret = backend->init(config, &backend->state);

    if (ret == 0) {
        backend->name = strdup(name);
        DBG("%s: success", name);
    }

    return ret;
}

int unicapture_try_backends(cap_backend_config_t* config, capture_backend_t* backend, char** candidates) {
    for (int i = 0; candidates[i] != NULL; i++) {
        if (unicapture_init_backend(config, backend, candidates[i]) == 0) return 0;
    }

    return -1;
}

void* unicapture_vsync_handler(void* data) {
    unicapture_state_t* this = (unicapture_state_t*) data;

    INFO("vsync thread starting...");

    while (this->vsync_thread_running) {
        if (this->video_capture && this->video_capture->wait) {
            this->video_capture->wait(this->video_capture->state);
        } else {
            DBG("Using fallback wait...");
            usleep (1000000 / 30);
        }
        pthread_mutex_lock(&this->vsync_lock);
        pthread_cond_signal(&this->vsync_cond);
        pthread_mutex_unlock(&this->vsync_lock);
    }

    INFO("vsync thread finished");
}

int unicapture_run(unicapture_state_t* this) {
    capture_backend_t* ui_capture = this->ui_capture;
    capture_backend_t* video_capture = this->video_capture;

    bool ui_capture_ready = ui_capture != NULL;
    bool video_capture_ready = video_capture != NULL;

    uint64_t framecounter = 0;
    uint64_t framecounter_start = getticks_us();

    converter_t ui_converter;
    converter_t video_converter;

    converter_init(&ui_converter);
    converter_init(&video_converter);

    if (ui_capture_ready) {
        ui_capture->start(ui_capture->state);
    }

    if (video_capture_ready) {
        video_capture->start(video_capture->state);
    }

    pthread_mutex_init(&this->vsync_lock, NULL);
    pthread_cond_init(&this->vsync_cond, NULL);

    uint8_t* blended_frame = NULL;
    uint8_t* final_frame = NULL;

    this->vsync_thread_running = true;
    pthread_create(&this->vsync_thread, NULL, unicapture_vsync_handler, this);

    while (true) {
        int ret = 0;
        uint64_t frame_start = getticks_us();

        pthread_mutex_lock(&this->vsync_lock);
        pthread_cond_wait(&this->vsync_cond, &this->vsync_lock);
        pthread_mutex_unlock(&this->vsync_lock);

        uint64_t frame_wait = getticks_us();

        frame_info_t ui_frame = {PIXFMT_INVALID};
        frame_info_t ui_frame_converted = {PIXFMT_INVALID};
        frame_info_t video_frame = {PIXFMT_INVALID};
        frame_info_t video_frame_converted = {PIXFMT_INVALID};

        // Capture frames
        if (ui_capture_ready) {
            if ((ret = ui_capture->acquire_frame(ui_capture->state, &ui_frame)) != 0) {
                ui_frame.pixel_format = PIXFMT_INVALID;
            }
        }

        if (video_capture_ready) {
            if ((ret = video_capture->acquire_frame(video_capture->state, &video_frame)) != 0) {
                DBG("video_capture acquire_frame failed: %d", ret);
                video_frame.pixel_format = PIXFMT_INVALID;
            }
        }

        uint64_t frame_acquired = getticks_us();
        // TODO fastpaths handling?

        // Convert frame to suitable video formats
        if (ui_frame.pixel_format != PIXFMT_INVALID) {
            converter_run(&ui_converter, &ui_frame, &ui_frame_converted, PIXFMT_ARGB);
        }

        if (video_frame.pixel_format != PIXFMT_INVALID) {
            converter_run(&video_converter, &video_frame, &video_frame_converted, PIXFMT_ARGB);
        }

        uint64_t frame_converted = getticks_us();

        // Blend frames and prepare for sending
        if (video_frame_converted.pixel_format != PIXFMT_INVALID && ui_frame_converted.pixel_format != PIXFMT_INVALID) {
            int width = video_frame_converted.width;
            int height = video_frame_converted.height;

            blended_frame = realloc(blended_frame, width * height * 4);
            final_frame = realloc(final_frame, width * height * 3);

            ARGBBlend(
                ui_frame_converted.planes[0].buffer,
                ui_frame_converted.planes[0].stride,
                video_frame_converted.planes[0].buffer,
                video_frame_converted.planes[0].stride,
                blended_frame,
                4 * width,
                width,
                height
            );
        } else if (ui_frame_converted.pixel_format != PIXFMT_INVALID) {
            // TODO
        } else if (video_frame_converted.pixel_format != PIXFMT_INVALID) {
            // TODO
        } else {
            WARN("No valid frame to send...");
        }


        uint64_t frame_processed = getticks_us();

        uint64_t frame_sent = getticks_us();

        if (ui_frame.pixel_format != PIXFMT_INVALID) {
            ui_capture->release_frame(ui_capture->state, &ui_frame);
        }

        if (video_frame.pixel_format != PIXFMT_INVALID) {
            video_capture->release_frame(video_capture->state, &video_frame);
        }

        framecounter += 1;
        if (framecounter >= 60) {
            double fps = (framecounter * 1000000.0) / (getticks_us() - framecounter_start);
            INFO("Framerate: %.6f FPS; timings - wait: %lldus, acquire: %lldus, convert: %lldus, process; %lldus, send: %lldus",
                    fps, frame_wait - frame_start, frame_acquired - frame_wait, frame_converted - frame_acquired, frame_processed - frame_converted, frame_sent - frame_processed);

            INFO("        UI: pixfmt: %d; %dx%d", ui_frame.pixel_format, ui_frame.width, ui_frame.height);
            INFO("     VIDEO: pixfmt: %d; %dx%d", video_frame.pixel_format, video_frame.width, video_frame.height);
            INFO("CONV    UI: pixfmt: %d; %dx%d", ui_frame_converted.pixel_format, ui_frame_converted.width, ui_frame_converted.height);
            INFO("CONV VIDEO: pixfmt: %d; %dx%d", video_frame_converted.pixel_format, video_frame_converted.width, video_frame_converted.height);

            framecounter = 0;
            framecounter_start = getticks_us();
        }
    }

    INFO("Shutting down...");

    if (this->vsync_thread_running) {
        DBG("Waiting for vsync thread to finish...");
        this->vsync_thread_running = false;
        pthread_join(this->vsync_thread, NULL);
    }

    if (ui_capture_ready) {
        DBG("Terminating UI capture...");
        ui_capture->terminate(ui_capture->state);
    }

    if (video_capture_ready) {
        DBG("Terminating Video capture...");
        video_capture->terminate(video_capture->state);
    }

    if (final_frame != NULL) {
        free(final_frame);
        final_frame = NULL;
    }

    if (blended_frame != NULL) {
        free(blended_frame);
        blended_frame = NULL;
    }

    converter_release(&ui_converter);
    converter_release(&video_converter);
}