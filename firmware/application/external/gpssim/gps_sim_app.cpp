/*
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "gps_sim_app.hpp"
#include "string_format.hpp"

#include "ui_fileman.hpp"
#include "io_file.hpp"
#include "metadata_file.hpp"
#include "utility.hpp"
#include "file_path.hpp"

#include "baseband_api.hpp"
#include "portapack.hpp"
#include "portapack_persistent_memory.hpp"

using namespace portapack;
namespace fs = std::filesystem;

namespace ui::external_app::gpssim {

void GpsSimAppView::set_ready() {
    ready_signal = true;
}

void GpsSimAppView::on_file_changed(const fs::path& new_file_path) {
    file_path = new_file_path;
    File::Size file_size{};

    {  // Get the size of the data file.
        File data_file;
        auto error = data_file.open(file_path);
        if (error) {
            file_error();
            return;
        }

        file_size = data_file.size();
    }

    // Get original record frequency if available.
    auto metadata_path = get_metadata_path(file_path);
    auto metadata = read_metadata_file(metadata_path);

    if (metadata) {
        field_frequency.set_value(metadata->center_frequency);
        transmitter_model.set_sampling_rate(metadata->sample_rate);
        // Update sample rate fields from metadata
        update_sample_rate_fields_from_hz(metadata->sample_rate);
    }

    // UI Fixup.
    progressbar.set_max(file_size);
    text_filename.set(truncate(file_path.filename().string(), 12));

    auto duration = ms_duration(file_size, transmitter_model.sampling_rate(), 2);
    text_duration.set(to_string_time_ms(duration));

    // TODO: fix in UI framework with 'try_focus()'?
    // Hack around focus getting called by ctor before parent is set.
    if (parent())
        button_play.focus();
}

void GpsSimAppView::on_tx_progress(const uint32_t progress) {
    progressbar.set_value(progress);
}

void GpsSimAppView::focus() {
    button_open.focus();
}

void GpsSimAppView::file_error() {
    nav_.display_modal("Error", "File read error.");
}

bool GpsSimAppView::is_active() const {
    return (bool)replay_thread;
}

void GpsSimAppView::toggle() {
    if (is_active()) {
        stop(false);
    } else {
        start();
    }
}

void GpsSimAppView::start() {
    stop(false);

    std::unique_ptr<stream::Reader> reader;

    auto p = std::make_unique<FileReader>();
    auto open_error = p->open(file_path);
    if (open_error.is_valid()) {
        file_error();
    } else {
        reader = std::move(p);
    }

    if (reader) {
        button_play.set_bitmap(&bitmap_stop);

        replay_thread = std::make_unique<ReplayThread>(
            std::move(reader),
            read_size, buffer_count,
            &ready_signal,
            [](uint32_t return_code) {
                ReplayThreadDoneMessage message{return_code};
                EventDispatcher::send_message(message);
            });
    }

    transmitter_model.enable();
}

void GpsSimAppView::stop(const bool do_loop) {
    if (is_active())
        replay_thread.reset();

    if (do_loop && check_loop.value()) {
        start();
    } else {
        transmitter_model.disable();
        button_play.set_bitmap(&bitmap_play);
    }

    ready_signal = false;
}

void GpsSimAppView::handle_replay_thread_done(const uint32_t return_code) {
    if (return_code == ReplayThread::END_OF_FILE) {
        stop(true);
    } else if (return_code == ReplayThread::READ_ERROR) {
        stop(false);
        file_error();
    }

    progressbar.set_value(0);
}

GpsSimAppView::GpsSimAppView(
    NavigationView& nav)
    : nav_(nav) {
    // baseband::run_image(portapack::spi_flash::image_tag_gps);
    baseband::run_prepared_image(portapack::memory::map::m4_code.base());

    add_children({
        &button_open,
        &text_filename,
        &field_sample_rate_int,
        &text_sample_rate_dot,
        &field_sample_rate_dec,
        &text_sample_rate_unit,
        &text_duration,
        &progressbar,
        &field_frequency,
        &tx_view,  // now it handles previous rfgain, rfamp.
        &check_loop,
        &button_play,
        &waterfall,
    });

    field_frequency.set_step(5000);

    // Set default sample rate from radio state
    update_sample_rate_fields_from_hz(transmitter_model.sampling_rate());

    // Handle sample rate changes (integer part)
    field_sample_rate_int.on_change = [this](int32_t) {
        set_sample_rate_from_fields();
    };

    // Handle sample rate changes (decimal part)
    field_sample_rate_dec.on_change = [this](int32_t) {
        set_sample_rate_from_fields();
    };

    button_play.on_select = [this](ImageButton&) {
        this->toggle();
    };

    button_open.on_select = [this, &nav](Button&) {
        auto open_view = nav.push<FileLoadView>(".C8");
        ensure_directory(gps_dir);
        open_view->push_dir(gps_dir);
        open_view->on_changed = [this](std::filesystem::path new_file_path) {
            on_file_changed(new_file_path);
        };
    };
}

GpsSimAppView::~GpsSimAppView() {
    transmitter_model.disable();
    baseband::shutdown();
}

void GpsSimAppView::on_hide() {
    // TODO: Terrible kludge because widget system doesn't notify Waterfall that
    // it's being shown or hidden.
    if (is_active())
        stop(false);
    waterfall.on_hide();
    View::on_hide();
}

void GpsSimAppView::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);

    const ui::Rect waterfall_rect{0, header_height, new_parent_rect.width(), new_parent_rect.height() - header_height};
    waterfall.set_parent_rect(waterfall_rect);
}

void GpsSimAppView::set_sample_rate_from_fields() {
    // Get values from both fields and combine (e.g., 2.6 MHz = 2600000 Hz)
    int32_t int_part = field_sample_rate_int.value();
    int32_t dec_part = field_sample_rate_dec.value();
    uint32_t sample_rate_hz = (int_part * 1000000) + (dec_part * 100000);

    transmitter_model.set_sampling_rate(sample_rate_hz);

    // Update duration if file is loaded
    if (!file_path.empty()) {
        File data_file;
        if (!data_file.open(file_path)) {
            auto file_size = data_file.size();
            auto duration = ms_duration(file_size, transmitter_model.sampling_rate(), 2);
            text_duration.set(to_string_time_ms(duration));
        }
    }
}

void GpsSimAppView::update_sample_rate_fields_from_hz(uint32_t sample_rate_hz) {
    // Convert Hz to MHz with one decimal place (e.g., 2600000 Hz = 2.6 MHz)
    int32_t int_part = sample_rate_hz / 1000000;           // Integer part (MHz)
    int32_t dec_part = (sample_rate_hz % 1000000) / 100000;  // First decimal digit

    field_sample_rate_int.set_value(int_part);
    field_sample_rate_dec.set_value(dec_part);
}

} /* namespace ui::external_app::gpssim */
