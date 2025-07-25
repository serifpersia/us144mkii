#ifndef ALSACONTROLLER_H
#define ALSACONTROLLER_H

#include <string>
#include <optional>

class AlsaController
{
public:
    AlsaController(const std::string& target_card_name = "US-144MKII");

    std::optional<std::string> getCardId() const;
    int getCardNumber() const;
    bool isCardFound() const;

    long getControlValue(const std::string& control_name);
    bool setControlValue(const std::string& control_name, long value);
    std::string readSysfsAttr(const std::string& attr_name);

private:
    std::string m_card_id_str;
    int m_card_num = -1;
    bool m_card_found = false;
};

#endif