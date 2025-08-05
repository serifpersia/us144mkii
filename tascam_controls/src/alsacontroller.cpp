#include "alsacontroller.h"
#include <alsa/asoundlib.h>
#include <fstream>
#include <iostream>
#include <vector>

AlsaController::AlsaController(const std::vector<std::string>& target_card_names)
{
    int card = -1;
    if (snd_card_next(&card) < 0 || card < 0) {
        std::cerr << "No sound cards found." << std::endl;
        return;
    }

    while (card >= 0) {
        char* long_name = nullptr;
        snd_card_get_longname(card, &long_name);
        if (long_name) {
            bool found_match_and_freed = false;
            for (const auto& name : target_card_names) {
                if (std::string(long_name).find(name) != std::string::npos) {
                    m_card_num = card;
                    m_card_id_str = "hw:" + std::to_string(card);
                    m_card_found = true;
                    free(long_name);
                    found_match_and_freed = true;
                    break;
                }
            }
            if (!found_match_and_freed) {
                free(long_name);
            }
        }

        if (m_card_found) break;

        if (snd_card_next(&card) < 0) {
            break;
        }
    }

    if (!m_card_found) {
        std::cerr << "Target sound card(s) not found." << std::endl;
    }
}

std::optional<std::string> AlsaController::getCardId() const {
    if (m_card_found) {
        return m_card_id_str;
    }
    return std::nullopt;
}

int AlsaController::getCardNumber() const {
    return m_card_num;
}

bool AlsaController::isCardFound() const {
    return m_card_found;
}

long AlsaController::getControlValue(const std::string& control_name) {
    if (!m_card_found) return 0;

    snd_ctl_t *handle;
    if (snd_ctl_open(&handle, m_card_id_str.c_str(), 0) < 0) return 0;

    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_t *control;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&control);

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, control_name.c_str());

    snd_ctl_elem_value_set_id(control, id);

    if (snd_ctl_elem_read(handle, control) < 0) {
        snd_ctl_close(handle);
        return 0;
    }

    long value = snd_ctl_elem_value_get_integer(control, 0);
    snd_ctl_close(handle);
    return value;
}

bool AlsaController::setControlValue(const std::string& control_name, long value) {
    if (!m_card_found) return false;

    snd_ctl_t *handle;
    if (snd_ctl_open(&handle, m_card_id_str.c_str(), 0) < 0) return false;

    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_t *control;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&control);

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, control_name.c_str());
    snd_ctl_elem_value_set_id(control, id);

    if (snd_ctl_elem_read(handle, control) < 0) {
        snd_ctl_close(handle);
        return false;
    }

    snd_ctl_elem_value_set_integer(control, 0, value);

    if (snd_ctl_elem_write(handle, control) < 0) {
        snd_ctl_close(handle);
        return false;
    }

    snd_ctl_close(handle);
    return true;
}

std::string AlsaController::readSysfsAttr(const std::string& attr_name) {
    if (!m_card_found) return "N/A";

    std::string path = "/sys/class/sound/card" + std::to_string(m_card_num) + "/device/" + attr_name;
    std::ifstream file(path);
    if (file.is_open()) {
        std::string line;
        std::getline(file, line);
        return line;
    }
    return "N/A";
}