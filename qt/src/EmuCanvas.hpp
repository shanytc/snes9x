#pragma once
#include <QWidget>
#include <QImage>
#include "common/video/std_chrono_throttle.hpp"

class EmuApplication;
class EmuConfig;

/* The shape the picture is held to, as a width:height pair. Game Boy content
 * keeps its own -- the aspect setting names the shape, as on win32, so square
 * pixels leave the 160x144 picture unstretched and the other choices give it
 * the same display shape a SNES picture gets. Shared by the blit and by
 * View->Set Size, so a window sized from the menu has no bars.
 */
void S9xQtDisplayAspect(EmuConfig *config, int *num, int *den);

class EmuCanvas : public QWidget
{
  public:
    EmuCanvas(EmuApplication &app, QWidget *parent);

    virtual void deinit() = 0;
    virtual void draw() = 0;
    void paintEvent(QPaintEvent *) override = 0;
    virtual void recreateUIAssets() {}
    void output(uint8_t *buffer, int width, int height, QImage::Format format, int bytes_per_line, double frame_rate);
    void throttle();
    double get_late_frames();
    void resizeEvent(QResizeEvent *event) override = 0;

    virtual std::vector<std::string> getDeviceList()
    {
        return std::vector<std::string>{ "Default" };
    }

    bool ready()
    {
        return output_data.ready;
    }

    QRect applyAspect(const QRect &viewport);

    struct Parameter
    {
        bool operator==(const Parameter &other) const
        {
            if (name == other.name &&
                id   == other.id &&
                min  == other.min &&
                max  == other.max &&
                val  == other.val &&
                step == other.step &&
                significant_digits == other.significant_digits)
                return true;
            return false;
        };

        std::string name;
        std::string id;
        float min;
        float max;
        float val;
        float step;
        int significant_digits;
    };

    struct ShaderProperties
    {
        std::string *name;
        std::vector<Parameter> *parameters;
    };

    virtual void showParametersDialog() {};
    virtual void shaderChanged() {};
    virtual void saveParameters(std::string filename) {};
    virtual void signalInputStage() {};

    struct
    {
        bool ready;
        uint8_t *buffer;
        int width;
        int height;
        QImage::Format format;
        int bytes_per_line;
        double frame_rate;
    } output_data;

    QWidget *parent{};
    EmuApplication &app;
    EmuConfig &config;
    Throttle throttle_object;
};