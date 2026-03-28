idf_component_register(
    SRCS "main.cpp"
         "hal/serial_io.cpp"
         "audio/audio_buffer.cpp"
         "audio/post_processor.cpp"
         "audio/coherence_filter.cpp"
         "audio/vad.cpp"
         "config/cal_store.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp-sr esp_psram nvs_flash esp-dsp
)
