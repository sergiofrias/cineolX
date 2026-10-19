#include "Editor.hpp"
#include "CineolUIData.h"
#include <algorithm>

namespace {
constexpr int panel_width=1640,panel_height=1240;
constexpr int minimum_editor_width=984,minimum_editor_height=744;
constexpr float status_text_pitch=2.1f;
constexpr int variable_fader_x=64,fader_step=164,global_fader_x=1098,global_fader_step=fader_step;
constexpr int fader_y=370,fader_height=525;
constexpr int display_global_x=global_fader_x-variable_fader_x,display_divider_x=display_global_x-30;
constexpr const char* captions[]={"BASS","MID","CROSSOVER","TREBLE","DEPTH","PRE-DELAY",
    "DIFFUSION","INPUT","MIX"};
const juce::Colour ink(0xff171612),led(0xffff4035),led_text(0xffff7560);
juce::Font font(float height,bool bold=false) {
    return juce::Font(juce::FontOptions("Arial",height,bold?juce::Font::bold:juce::Font::plain));
}
juce::Component* menuParent(juce::Component* target) {
    for(auto* component=target;component;component=component->getParentComponent())
        if(component->getComponentID()=="cineol_panel") return component;
    return target?target->findParentComponentOfClass<juce::AudioProcessorEditor>():nullptr;
}
void dismissEditorMenus(juce::Component& editor) {
    // Never dismiss another plugin instance's menus. Parented menus are
    // identifiable even when the host hides or reparents the editor.
    for(int i=juce::Component::getNumCurrentlyModalComponents();--i>=0;)
        if(auto* modal=juce::Component::getCurrentlyModalComponent(i))
            if(modal->getName()=="menu" && editor.isParentOf(modal)) {
                modal->setVisible(false);modal->exitModalState(0);
            }
}
// Only the graphics thread loads/decodes these embedded assets.
juce::Image loadImage(const char* name) {
    int size=0;auto* data=CineolUIData::getNamedResource(name,size);
    return juce::ImageFileFormat::loadFrom(data,size_t(size));
}
juce::Image trimTransparentPadding(const juce::Image& source) {
    if(!source.isValid()) return {};
    int left=source.getWidth(),top=source.getHeight(),right=0,bottom=0;
    juce::Image::BitmapData pixels(source,juce::Image::BitmapData::readOnly);
    for(int y=0;y<source.getHeight();++y) for(int x=0;x<source.getWidth();++x)
        if(pixels.getPixelColour(x,y).getAlpha()>32) {
            left=std::min(left,x);top=std::min(top,y);right=std::max(right,x);bottom=std::max(bottom,y);
        }
    return right>=left && bottom>=top?source.getClippedImage({left,top,right-left+1,bottom-top+1}):source;
}
// Preserve the source bezel's corner radii and border thickness when the
// display grows. Stretch only its glass and straight frame sections.
void drawBezel(juce::Graphics& g,const juce::Image& image,juce::Rectangle<int> source,
               juce::Rectangle<int> destination,int border) {
    juce::Graphics::ScopedSaveState saved(g);
    g.setOpacity(1.0f);
    juce::Path outline;outline.addRoundedRectangle(destination.toFloat(),float(border));
    g.reduceClipRegion(outline);
    const int sx[]={source.getX(),source.getX()+border,source.getRight()-border,source.getRight()};
    const int sy[]={source.getY(),source.getY()+border,source.getBottom()-border,source.getBottom()};
    const int dx[]={destination.getX(),destination.getX()+border,destination.getRight()-border,destination.getRight()};
    const int dy[]={destination.getY(),destination.getY()+border,destination.getBottom()-border,destination.getBottom()};
    for(int y=0;y<3;++y) for(int x=0;x<3;++x)
        g.drawImage(image,dx[x],dy[y],dx[x+1]-dx[x],dy[y+1]-dy[y],sx[x],sy[y],sx[x+1]-sx[x],sy[y+1]-sy[y]);
}
// The attached Slider always holds the actual parameter. Only the cap's
// painted position moves; animation never emits a parameter/automation event.
class MotorFader final : public juce::Slider,private juce::Timer {
public:
    void connect(NativeHallProcessor& processor) {
        processor_=&processor;revision_=processor.presetRecallRevision();snap();
    }
    double visualProportion() const {return position_;}
    void animateFrom(double position) {
        position_=start_=position;target_=parked_?0:valueToProportionOfLength(getValue());
        began_=juce::Time::getMillisecondCounterHiRes();moving_=std::abs(target_-start_)>0.00001;
        if(moving_) startTimerHz(60);else stopTimer();publish();
    }
    void setParked(bool parked) {if(parked_!=parked) {parked_=parked;snap();}}
    void mouseDown(const juce::MouseEvent& event) override {snap();juce::Slider::mouseDown(event);}
    void valueChanged() override {
        const auto revision=processor_?processor_->presetRecallRevision():revision_;
        if(processor_ && revision!=revision_ && processor_->presetRecallInProgress()) {
            advance();revision_=revision;start_=position_;target_=parked_?0:valueToProportionOfLength(getValue());
            began_=juce::Time::getMillisecondCounterHiRes();moving_=std::abs(target_-start_)>0.00001;
            if(moving_) startTimerHz(60);else stopTimer();publish();
        } else snap();
    }
private:
    void snap() {
        stopTimer();moving_=false;position_=parked_?0:valueToProportionOfLength(getValue());
        if(processor_) revision_=processor_->presetRecallRevision();publish();
    }
    void advance() {
        if(!moving_) return;
        const double t=std::clamp((juce::Time::getMillisecondCounterHiRes()-began_)/320.0,0.0,1.0);
        position_=start_+(target_-start_)*t*t*(3-2*t);
        if(t>=1) {position_=target_;moving_=false;stopTimer();}
    }
    void publish() {
        // Diagnostics also let UI checks verify motion without changing values.
        getProperties().set("motor_position",position_);getProperties().set("motor_moving",moving_);repaint();
    }
    void timerCallback() override {advance();publish();}
    NativeHallProcessor* processor_=nullptr;
    unsigned revision_=0;
    double position_=0,start_=0,target_=0,began_=0;
    bool moving_=false,parked_=false;
};
class InstrumentLook : public juce::LookAndFeel_V4 {
public:
    juce::Component* getParentComponentForMenuOptions(const juce::PopupMenu::Options& options) override {
        if(auto* parent=menuParent(options.getTargetComponent())) return parent;
        return options.getParentComponent();
    }
    InstrumentLook():cap_(loadImage("fadercap_png")),material_(loadImage("menumetal_png")) {
        inactive_cap_=cap_.createCopy();
        juce::Image::BitmapData pixels(inactive_cap_,juce::Image::BitmapData::readWrite);
        for(int y=0;y<pixels.height;++y) for(int x=0;x<pixels.width;++x)
            pixels.setPixelColour(x,y,pixels.getPixelColour(x,y).withSaturation(0).withMultipliedBrightness(0.58f));
        setColour(juce::ComboBox::textColourId,juce::Colour(0xfff1ece0));
        // JUCE uses this colour's alpha to choose a transparent native menu
        // window. The rounded metal surface itself is painted fully opaque.
        setColour(juce::PopupMenu::backgroundColourId,juce::Colours::transparentBlack);
        setColour(juce::PopupMenu::textColourId,ink);
        setColour(juce::PopupMenu::highlightedBackgroundColourId,juce::Colour(0xff6d2c28));
        setColour(juce::PopupMenu::highlightedTextColourId,juce::Colours::white);
        setColour(juce::TooltipWindow::backgroundColourId,juce::Colour(0xffece7db));
        setColour(juce::TooltipWindow::textColourId,ink);
        setColour(juce::AlertWindow::backgroundColourId,juce::Colour(0xffe9e3d6));
        setColour(juce::AlertWindow::textColourId,ink);
        setColour(juce::TextButton::buttonColourId,juce::Colour(0xff302c25));
        setColour(juce::TextButton::textColourOffId,juce::Colour(0xfff1ece0));
        setColour(juce::TextEditor::backgroundColourId,juce::Colour(0xff180806));
        setColour(juce::TextEditor::textColourId,led_text);
        setColour(juce::TextEditor::outlineColourId,juce::Colour(0xff777064));
        setColour(juce::TextEditor::focusedOutlineColourId,led_text);
        setColour(juce::TextEditor::highlightColourId,juce::Colour(0xff73332b));
        setColour(juce::TextEditor::highlightedTextColourId,juce::Colour(0xffffe6d5));
        setColour(juce::CaretComponent::caretColourId,led_text);
        setColour(juce::ScrollBar::thumbColourId,ink.withAlpha(0.45f));
        setColour(juce::ScrollBar::trackColourId,juce::Colours::transparentBlack);
    }
    void drawMetalSurface(juce::Graphics& g,juce::Rectangle<float> bounds,float radius=7) {
        juce::Graphics::ScopedSaveState saved(g);
        g.setOpacity(1.0f);
        juce::Path shape;shape.addRoundedRectangle(bounds,radius);
        g.reduceClipRegion(shape);
        // Independent, evenly worn material at a fixed grain scale. Normal
        // menus/dialogs use a single crop; unusually large surfaces can wrap
        // without stretching scratches or inheriting faceplate wear bands.
        g.setTiledImageFill(material_,int(bounds.getX()),int(bounds.getY()),1.0f);
        g.fillRect(bounds);
        g.setColour(ink.withAlpha(0.85f));g.drawRoundedRectangle(bounds.reduced(1),radius,2);
        g.setColour(juce::Colour(0xfff3edde));g.drawRoundedRectangle(bounds.reduced(3),std::max(0.0f,radius-2),1);
    }
    void drawScreenHeader(juce::Graphics& g,const juce::String& title,juce::Rectangle<float> bounds,float height) {
        g.setColour(juce::Colour(0xff190503));g.fillRoundedRectangle(bounds,3);
        g.setColour(juce::Colour(0xff777064));g.drawRoundedRectangle(bounds,3,1);
        g.setColour(led_text);g.setFont(font(height));
        g.drawText(title.toUpperCase(),bounds.reduced(12,2),juce::Justification::centredLeft);
    }
    void drawPopupMenuBackground(juce::Graphics& g,int width,int height) override {
        drawMetalSurface(g,{0,0,float(width),float(height)},juce::Desktop::canUseSemiTransparentWindows()?7.0f:0.0f);
    }
    int getPopupMenuBorderSize() override {return 4;}
    juce::Font getPopupMenuFont() override {return font(18);}
    void drawPopupMenuSectionHeader(juce::Graphics& g,const juce::Rectangle<int>& area,const juce::String& title) override {
        // JUCE adds a four-pixel border above the first row and on either
        // side of its custom header. Two more pixels give equal six-pixel insets.
        drawScreenHeader(g,title,area.toFloat().reduced(2,2),16);
    }
    juce::Font getAlertWindowTitleFont() override {return font(20);}
    juce::Font getAlertWindowMessageFont() override {return font(16);}
    juce::Font getAlertWindowFont() override {return font(16);}
    juce::Font getTextButtonFont(juce::TextButton&,int height) override {return font(std::min(18.0f,float(height)*0.48f));}
    void drawButtonBackground(juce::Graphics& g,juce::Button& button,const juce::Colour&,bool highlighted,bool down) override {
        const auto bounds=button.getLocalBounds().toFloat().reduced(1);
        g.setColour(juce::Colour(0xff696256));g.fillRoundedRectangle(bounds,4);
        g.setColour(juce::Colour(0xffeee6d4));g.drawRoundedRectangle(bounds,4,1);
        g.setGradientFill(juce::ColourGradient(juce::Colour(down?0xff170705:highlighted?0xff6d2c28:0xff363127),
            0,bounds.getY(),juce::Colour(0xff181510),0,bounds.getBottom(),false));
        g.fillRoundedRectangle(bounds.reduced(3),2);
        if(button.hasKeyboardFocus(true)) {g.setColour(led_text);g.drawRoundedRectangle(bounds.reduced(2),3,1);}
    }
    void drawAlertBox(juce::Graphics& g,juce::AlertWindow& alert,const juce::Rectangle<int>&,juce::TextLayout& layout) override {
        drawMetalSurface(g,alert.getLocalBounds().toFloat());
        auto area=alert.getLocalBounds().reduced(18);
        g.setColour(juce::Colour(0xff190503));g.fillRoundedRectangle(float(area.getX()),22,float(area.getWidth()),36,3);
        g.setColour(juce::Colour(0xff777064));g.drawRoundedRectangle(float(area.getX()),22,float(area.getWidth()),36,3,1);
        for(int line=0;line<layout.getNumLines();++line)
            for(auto* run:layout.getLine(line).runs) run->colour=line==0?led_text:ink;
        layout.draw(g,{float(area.getX()+12),30,float(area.getWidth()-24),float(alert.getHeight()-70)});
    }
    int getSliderThumbRadius(juce::Slider&) override {return 33;}
    juce::Slider::SliderLayout getSliderLayout(juce::Slider& s) override {
        juce::Slider::SliderLayout l;
        // One rail/cap geometry for every page and enabled state. Leave room
        // for the cap and its shadow at both ends, without a numeric text box.
        l.sliderBounds={0,33,s.getWidth(),s.getHeight()-80};return l;
    }
    void drawLinearSlider(juce::Graphics& g,int x,int y,int width,int height,float pos,float,float,
                          juce::Slider::SliderStyle,juce::Slider& s) override {
        if(auto* motor=dynamic_cast<MotorFader*>(&s)) pos=float(y)+float(height)*float(1-motor->visualProportion());
        const float cx=float(x)+float(width)/2;
        g.setOpacity(1.0f);
        juce::Rectangle<float> rail(cx-10,float(y-33),20,float(height+66));
        g.setColour(juce::Colour(0xff5c5951));g.fillRoundedRectangle(rail.expanded(3),5);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff090907),cx-10,0,
                                              juce::Colour(0xff37342d),cx+10,0,false));
        g.fillRoundedRectangle(rail,4);
        g.setColour(juce::Colour(0xffddd7c9));g.drawRoundedRectangle(rail.expanded(3),5,1);
        for(int i=0;i<=20;++i) {
            const float ty=float(y)+float(height)*float(i)/20;
            constexpr float length=11;
            const bool major=i%10==0;
            const float thickness=major?3.0f:1.0f;
            g.setColour(ink.withAlpha(major?0.75f:0.5f));
            g.fillRect(cx-23-length,ty-thickness/2,length,thickness);
            g.fillRect(cx+23,ty-thickness/2,length,thickness);
        }
        const float capWidth = 136.0f;
        const float aspect = (cap_.isValid() && cap_.getHeight() > 0)
            ? float(cap_.getWidth()) / float(cap_.getHeight())
            : 1.92f;
        const float capHeight = capWidth / aspect;
        juce::Rectangle<float> cap(cx - capWidth * 0.5f, pos - capHeight * 0.5f, capWidth, capHeight);

        g.setOpacity(1.0f);
        g.drawImage(s.isEnabled() ? cap_ : inactive_cap_, cap, juce::RectanglePlacement::centred);
    }
    void drawComboBox(juce::Graphics& g,int width,int height,bool down,int,int,int,int,juce::ComboBox& c) override {
        auto r=juce::Rectangle<float>(1,1,float(width-2),float(height-2));
        g.setColour(juce::Colour(0xff5d5a52));g.fillRoundedRectangle(r,5);
        g.setColour(juce::Colour(0xffded8c8));g.drawRoundedRectangle(r,5,1.5f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(down?0xff151411:0xff34312a),0,4,
                                             juce::Colour(0xff1b1915),0,float(height),false));
        g.fillRoundedRectangle(r.reduced(4),3);
        if(c.hasKeyboardFocus(true)) {g.setColour(juce::Colour(0xff9f4d3d));g.drawRoundedRectangle(r.reduced(1),4,2);}
        juce::Path arrow;float ax=float(width)-32,ay=float(height)/2;
        arrow.addTriangle(ax-8,ay-4,ax+8,ay-4,ax,ay+5);
        g.setColour(juce::Colour(0xffe7e0d2));g.fillPath(arrow);
    }
    juce::Font getComboBoxFont(juce::ComboBox&) override {return font(28);}
    void positionComboBoxText(juce::ComboBox& box,juce::Label& label) override {
        label.setBounds(18,1,box.getWidth()-70,box.getHeight()-2);label.setFont(getComboBoxFont(box));
    }
    void drawToggleButton(juce::Graphics& g,juce::ToggleButton& b,bool highlighted,bool down) override {
        juce::Graphics::ScopedSaveState saved(g);g.setOpacity(b.isEnabled()?1.0f:0.4f);
        juce::Rectangle<float> square(3,float(b.getHeight()-50)/2,50,50);
        g.setColour(juce::Colour(0xff545148));g.fillRoundedRectangle(square,5);
        g.setColour(juce::Colour(highlighted?0xfff6efdd:0xffd8d0bc));g.drawRoundedRectangle(square.reduced(2),4,1.5f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(down?0xff141310:0xff333029),0,square.getY(),
                                              juce::Colour(0xff1c1a16),0,square.getBottom(),false));
        g.fillRoundedRectangle(square.reduced(5),2);
        if(b.getToggleState() && b.isEnabled()) {
            juce::Path tick;tick.startNewSubPath(15,square.getY()+27);tick.lineTo(25,square.getY()+35);
            tick.lineTo(41,square.getY()+16);g.setColour(juce::Colour(0xfff2e8d0));
            g.strokePath(tick,juce::PathStrokeType(4,juce::PathStrokeType::curved,juce::PathStrokeType::rounded));
        }
        if(b.hasKeyboardFocus(true)) {g.setColour(juce::Colour(0xff873d32));g.drawRoundedRectangle(square.expanded(2),5,2);}
        g.setColour(ink);g.setFont(font(26));
        g.drawText(b.getButtonText(),76,0,b.getWidth()-78,b.getHeight(),juce::Justification::centredLeft);
    }
private:
    juce::Image cap_,inactive_cap_,material_;
};
// 5x7 LED alphabet; UI-only, so the native DSP has no display work to do.
const char* glyph(char c) {
    constexpr const char* letters[]={"0E11111F111111","1E11111E11111E","0F10101010100F","1E11111111111E",
        "1F10101E10101F","1F10101E101010","0F10101711110F","1111111F111111","0E04040404040E",
        "0702020212120C","11121418141211","1010101010101F","111B1515111111","11191513111111",
        "0E11111111110E","1E11111E101010","0E11111115120D","1E11111E141211","0F10100E01011E",
        "1F040404040404","1111111111110E","11111111110A04","11111115151B11","11110A040A1111",
        "11110A04040404","1F01020408101F"};
    constexpr const char* digits[]={"0E11131519110E","040C040404040E","0E11010204081F","1E01010E01011E",
        "02060A121F0202","1F10101E01011E","0E10101E11110E","1F010204080808","0E11110E11110E","0E11110F01010E"};
    if(c>='A' && c<='Z') return letters[c-'A'];
    if(c>='0' && c<='9') return digits[c-'0'];
    switch(c) {
        case '-':return "0000001F000000";case '.':return "00000000000C0C";
        case '%':return "19190204081313";case '/':return "01010204081010";
        case '*':return "00150E1F0E1500";
        case '&':return "0C12140C15120D";case '+':return "0000041F040000";
        case '(':return "02040808080402";case ')':return "08040202020408";
        default:return "00000000000000";
    }
}
void dotText(juce::Graphics& g,const juce::String& text,juce::Rectangle<float> area,float maxPitch,bool centred,juce::Colour colour=led_text,bool fit=true) {
    const float pitch=fit?std::min({maxPitch,area.getHeight()/7,area.getWidth()/float(std::max(1,text.length()*6-1))}):maxPitch;
    const float textWidth=float(text.length()*6-1)*pitch;
    float x=centred?area.getCentreX()-textWidth/2:area.getX();
    const float y=area.getCentreY()-3.5f*pitch;
    auto hex=[](char n){return n<='9'?n-'0':n-'A'+10;};
    for(auto c:text.toUpperCase()) {
        const char* bits=glyph(char(c));
        for(int row=0;row<7;++row) {
            unsigned mask=unsigned(hex(bits[row*2])*16+hex(bits[row*2+1]));
            for(int col=0;col<5;++col) if(mask&(1u<<(4-col))) {
                const float dx=x+float(col)*pitch,dy=y+float(row)*pitch;
                // Broader luminous cores keep small labels readable when the
                // editor is scaled down, while retaining the dotted LED face.
                g.setColour(led.withAlpha(0.18f));g.fillEllipse(dx-pitch*0.31f,dy-pitch*0.31f,pitch*1.2f,pitch*1.2f);
                g.setColour(colour);g.fillEllipse(dx-pitch*0.09f,dy-pitch*0.09f,pitch*0.76f,pitch*0.76f);
            }
        }
        x+=6*pitch;
    }
}
void statusText(juce::Graphics& g,const juce::String& text,juce::Rectangle<float> area,juce::Colour colour=led_text) {
    // Controller names, values and right-hand switches share one fixed font.
    // Abbreviate captions instead of silently shrinking individual strings.
    dotText(g,text,area,status_text_pitch,true,colour,false);
}
juce::String statusCaption(const juce::String& name) {
    auto text=name.toUpperCase().trim().replace("HF BANDWIDTH","HF BW").replace("HF BNDWTH","HF BW");
    // Firmware captions include padding before L/R; preserve the channel suffix.
    auto words=juce::StringArray::fromTokens(text," ",{});
    words.removeEmptyStrings();text=words.joinIntoString(" ");
    text=text.replace("REV STOP DLY","STOP DLY").replace("MID STOP DCY","MID STOP")
        .replace("MID STPDCY","MID STOP").replace("LF STOP DCY","LF STOP").replace("LF STP DCY","LF STOP")
        .replace("TREBLE DECAY","HF DECAY").replace("TREB DECAY","HF DECAY")
        .replace("CUTOFF","CUT").replace("CROSSOVER","XOVER").replace("DEFINITION","DEFINIT")
        .replace("FIN PDLY","F PDLY").replace("NOTE PITCH","PITCH").replace("RESONANCE","RESON");
    if(text.length()>10) text=text.replace("DIFFUSION","DIFF").replace("DECAY","DCY")
        .replace("LEVEL","LVL").replace("DELAY","DLY");
    return text;
}
class PresetButton final : public juce::Button,private juce::Timer {
public:
    PresetButton():Button("Preset") {
        setComponentID("preset");setTitle("Preset");setTooltip("Choose a preset from the bank, grouped by algorithm.");startTimerHz(30);
    }
    ~PresetButton() override {stopTimer();}
    void setPresetText(const juce::String& text) {setButtonText(text);began_=juce::Time::getMillisecondCounterHiRes();}
    void paintButton(juce::Graphics& g,bool highlighted,bool down) override {
        if(highlighted || down) {g.setColour(led.withAlpha(0.08f));g.fillRoundedRectangle(getLocalBounds().toFloat(),4);}
        auto text=getButtonText();
        bool fallback=false;for(auto c:text.toUpperCase())
            fallback|=!((c>='A' && c<='Z') || (c>='0' && c<='9') || juce::String(" -.%/*").containsChar(c));
        const float available=float(getWidth()-48);
        const float natural=fallback?juce::GlyphArrangement::getStringWidth(font(38),text):float(text.length()*6-1)*6.5f;
        const double overflow=std::max(0.0,double(natural-available));marquee_=overflow>0;
        double offset=0;
        if(marquee_) {
            constexpr double speed=60,pause=2;
            const double travel=overflow/speed,cycle=travel+2*pause;
            const double phase=std::fmod((juce::Time::getMillisecondCounterHiRes()-began_)/1000,cycle);
            offset=std::clamp((phase-pause)*speed,0.0,overflow);
        }
        getProperties().set("marquee_active",marquee_);getProperties().set("marquee_offset",offset);
        {
            juce::Graphics::ScopedSaveState saved(g);g.reduceClipRegion(0,0,int(available),getHeight());
            if(fallback) {
                g.setColour(led_text);g.setFont(font(38));
                g.drawText(text,int(-offset),0,int(std::ceil(std::max(natural,available))),getHeight(),juce::Justification::centredLeft);
            } else dotText(g,text,{float(-offset),0,std::max(natural,available),float(getHeight())},6.5f,false);
        }
        juce::Path arrow;const float x=float(getWidth()-23),y=float(getHeight())/2;
        arrow.addTriangle(x-10,y-5,x+10,y-5,x,y+6);g.setColour(led);g.fillPath(arrow);
        if(hasKeyboardFocus(true)) {g.setColour(led.withAlpha(0.5f));g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1),4,1);}
    }
private:
    void timerCallback() override {if(marquee_ && isShowing()) repaint();}
    double began_=juce::Time::getMillisecondCounterHiRes();bool marquee_=false;
};
class SavePresetButton final : public juce::Button {
public:
    SavePresetButton():Button("Save preset") {
        setComponentID("save_preset");setTitle("Save preset");setTooltip("Name and save the current algorithm and settings in the preset bank.");
    }
    void paintButton(juce::Graphics& g,bool highlighted,bool down) override {
        if(highlighted || down) {g.setColour(led.withAlpha(0.08f));g.fillRoundedRectangle(getLocalBounds().toFloat(),4);}
        juce::Path disk;disk.startNewSubPath(8,6);disk.lineTo(34,6);disk.lineTo(43,15);
        disk.lineTo(43,45);disk.lineTo(8,45);disk.closeSubPath();
        g.setColour(led);g.strokePath(disk,juce::PathStrokeType(2));
        g.drawRect(16,6,17,15,2);g.drawRect(16,30,20,15,2);g.drawLine(27,8,27,17,2);
        if(hasKeyboardFocus(true)) g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1),4,1);
    }
};
class LedButton final : public juce::Button {
public:
    explicit LedButton(const char* name,float pitch=2.5f,bool centred=true,const char* caption="")
        :Button(name),pitch_(pitch),centred_(centred),caption_(caption) {setComponentID(name);setTitle(name);}
    void setDimWhenUnselected(bool dim) {dim_when_unselected_=dim;repaint();}
    void paintButton(juce::Graphics& g,bool highlighted,bool down) override {
        if(highlighted || down) {g.setColour(led.withAlpha(0.08f));g.fillRoundedRectangle(getLocalBounds().toFloat(),4);}
        const float alpha=(isEnabled()?1.0f:0.35f)*(dim_when_unselected_ && !getToggleState()?0.35f:1.0f);
        const auto colour=led_text.withAlpha(alpha);
        if(caption_.isNotEmpty()) {
            auto value=getButtonText();
            if(value.startsWithIgnoreCase(caption_)) value=value.substring(caption_.length()).trim();
            statusText(g,caption_,{6,3,128,19},colour);
            statusText(g,value,{6,30,128,23},colour);
        } else dotText(g,getButtonText(),getLocalBounds().toFloat().reduced(4),pitch_,centred_,colour);
        if(hasKeyboardFocus(true)) {g.setColour(led);g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1),4,1);}
    }
private:
    float pitch_;bool centred_;juce::String caption_;bool dim_when_unselected_=false;
};
class Display final : public juce::Component {
public:
    Display(){
        setComponentID("display");setTitle("Preset, algorithm and parameter display");
        addAndMakeVisible(preset);preset.setBounds(12,18,910,50);
        addAndMakeVisible(save);save.setBounds(944,18,52,52);
        addAndMakeVisible(algorithm);algorithm.setBounds(12,87,608,38);
        addAndMakeVisible(dynamic);dynamic.setBounds(display_global_x,82,140,56);dynamic.setClickingTogglesState(true);
        dynamic.setTitle("XL Dynamic Decay");dynamic.setTooltip("Use STOP DECAY after a falling input level; REV STOP DLY sets the hold time. Saved in presets.");
        algorithm.setTooltip("Choose an algorithm. Selection switches its engine and clears the tail.");
        addAndMakeVisible(firmware);firmware.setBounds(display_global_x+2*global_fader_step,8,140,56);
        firmware.setTooltip("Engine used by the selected algorithm. Click to import ROMs.");
        for(unsigned i=0;i<outputs.size();++i) {addAndMakeVisible(outputs[i]);outputs[i].setBounds(display_global_x+int(i)*global_fader_step,8,140,56);}
        for(unsigned i=0;i<modes.size();++i) {addAndMakeVisible(modes[i]);modes[i].setBounds(display_global_x+int(i+1)*global_fader_step,82,140,56);modes[i].setClickingTogglesState(true);}
        for(unsigned i=0;i<page_buttons.size();++i) {
            auto& button=page_buttons[i];button.setButtonText(juce::String(i+1));button.setDimWhenUnselected(true);
            button.setTitle("Parameter page "+juce::String(i+1));button.setTooltip("Go directly to parameter page "+juce::String(i+1)+".");
            addChildComponent(button);
        }
    }
    void updatePreset(const juce::String& name,bool modified) {
        const auto text=name+(modified?" *":"");
        if(preset.getButtonText()!=text) {
            preset.setPresetText(text);preset.setName(text);
            preset.setTooltip(text+"\nBrowse all presets, or filter by one or more algorithms.");
        }
    }
    void update(int program,const juce::String& name,const juce::String& parameter,const juce::String& value) {
        if(program==program_ && name==name_ && parameter==parameter_ && value==value_) return;
        program_=program;name_=name;parameter_=parameter;value_=value;algorithm.setButtonText(name);
        setName(juce::String(program+1).paddedLeft('0',2)+" | "+name+" | "+parameter+" "+value);repaint();
    }
    void updateSlots(const std::array<juce::String,9>& names,const std::array<juce::String,9>& values) {
        if(names!=slot_names_ || values!=slot_values_) {
            slot_names_=names;slot_values_=values;
            for(unsigned slot=0;slot<names.size();++slot) {
                getProperties().set("slot_caption_"+juce::String(slot),statusCaption(names[slot]));
                getProperties().set("slot_value_"+juce::String(slot),values[slot]);
            }
            repaint();
        }
    }
    void updatePages(unsigned count,unsigned selected,bool enabled) {
        count=std::clamp(count,1u,unsigned(page_buttons.size()));
        constexpr int gap=8;
        const int width=std::min(40,(356-int(count-1)*gap)/int(count));
        const int start=996-(int(count)*width+int(count-1)*gap);
        for(unsigned i=0;i<page_buttons.size();++i) {
            auto& button=page_buttons[i];button.setVisible(i<count);button.setEnabled(enabled);
            button.setToggleState(i==selected,juce::dontSendNotification);
            button.setBounds(start+int(i)*(width+gap),87,width,38);
        }
    }
    void paint(juce::Graphics& g) override {
        dotText(g,"PRESET",{12,1,160,14},1.8f,false);
        dotText(g,"ALGORITHM",{12,78,210,12},1.5f,false);
        // The algorithm row is an interactive LED button.
        g.setColour(led.withAlpha(0.2f));g.drawVerticalLine(display_divider_x,0,240);
        g.drawHorizontalLine(154,0,1512);
        for(unsigned slot=0;slot<slot_names_.size();++slot) {
            const float x=slot<6?float(slot)*fader_step:float(global_fader_x+int(slot-6)*global_fader_step-variable_fader_x);
            statusText(g,statusCaption(slot_names_[slot]),{x+6,170,128,19});
            statusText(g,slot_values_[slot],{x+6,197,128,23});
        }
    }
    PresetButton preset;
    SavePresetButton save;
    LedButton firmware{"firmware",2.5f,true,"MODEL"},algorithm{"algorithm",4.5f,false};
    std::array<LedButton,2> outputs{LedButton{"output_l",2.5f,true,"LEFT"},LedButton{"output_r",2.5f,true,"RIGHT"}};
    std::array<LedButton,2> modes{LedButton{"mode_enh",2.5f,true,"MOD ENH"},LedButton{"decay_opt",2.5f,true,"DECAY OPT"}};
    LedButton dynamic{"xl_dynamic_decay",2.5f,true,"DYN DECAY"};
    std::array<LedButton,9> page_buttons{LedButton{"parameter_page_1",4.0f},LedButton{"parameter_page_2",4.0f},
        LedButton{"parameter_page_3",4.0f},LedButton{"parameter_page_4",4.0f},LedButton{"parameter_page_5",4.0f},
        LedButton{"parameter_page_6",4.0f},LedButton{"parameter_page_7",4.0f},LedButton{"parameter_page_8",4.0f},LedButton{"parameter_page_9",4.0f}};
private:
    int program_=2;
    juce::String name_,parameter_,value_;
    std::array<juce::String,9> slot_names_,slot_values_;
};

class SettingsButton final : public juce::Button {
public:
    SettingsButton():Button("Settings") {
        setComponentID("settings");setTitle("Settings");setClickingTogglesState(true);
        setTooltip("Open settings and fine tuning.");
    }
    void paintButton(juce::Graphics& g,bool highlighted,bool down) override {
        auto bounds=getLocalBounds().toFloat().reduced(5);
        juce::Path gear;const auto centre=bounds.getCentre();
        for(int i=0;i<32;++i) {
            const float angle=juce::MathConstants<float>::twoPi*float(i)/32;
            const float radius=(i%4==1 || i%4==2)?19.0f:15.0f;
            const auto point=centre+juce::Point<float>(std::sin(angle)*radius,std::cos(angle)*radius);
            if(i==0) gear.startNewSubPath(point);else gear.lineTo(point);
        }
        gear.closeSubPath();gear.addEllipse(centre.x-7,centre.y-7,14,14);
        gear.setUsingNonZeroWinding(false);
        g.setColour(getToggleState() || down?juce::Colour(0xff873d32):highlighted || hasKeyboardFocus(true)?juce::Colour(0xff474035):ink);g.fillPath(gear);
    }
};

class SettingsPanel final : public juce::Component {
public:
    explicit SettingsPanel(NativeHallProcessor& processor) {
        setComponentID("settings_panel");setName("Settings");setWantsKeyboardFocus(true);
        low_latency_.setComponentID("low_latency");low_latency_.setButtonText("Low latency");
        low_latency_.setTitle("Low latency");
        low_latency_.setTooltip("Direct dry signal with zero reported plugin latency. Reverb still passes through its filters and pre-delay.");
        addAndMakeVisible(low_latency_);
        attachment_=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.state,"low_latency",low_latency_);
        spillover_.setComponentID("spillover");spillover_.setButtonText("Spillover");spillover_.setTitle("Spillover");
        spillover_.setTooltip("Preserve the previous algorithm's tail with a smooth fade when switching 224 or 224 XL programs.");
        addAndMakeVisible(spillover_);
        duration_.setComponentID("spillover_time");duration_.setTitle("Tail fade time");
        duration_.setTooltip("Fade the previous tail over 1 to 10 seconds. This setting applies to the next transition.");
        for(int seconds=1;seconds<=10;++seconds) duration_.addItem(juce::String(seconds)+" s",seconds);
        addAndMakeVisible(duration_);
        spillover_.onStateChange=[this]{duration_.setEnabled(spillover_.getToggleState());};
        spillover_attachment_=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.state,"spillover",spillover_);
        duration_attachment_=std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processor.state,"spillover_time",duration_);
        duration_.setEnabled(spillover_.getToggleState());
    }
    void paint(juce::Graphics& g) override {
        auto bounds=getLocalBounds().toFloat().reduced(4);
        auto& look=static_cast<InstrumentLook&>(getLookAndFeel());
        look.drawMetalSurface(g,bounds);
        look.drawScreenHeader(g,"Settings",{28,20,float(getWidth()-56),38},25);
        g.setColour(ink);g.setFont(font(22));
        g.drawText("When enabled: zero-latency dry signal.",28,145,getWidth()-56,30,juce::Justification::centredLeft);
        g.drawText("Reverb and pre-delay keep their timing.",28,175,getWidth()-56,30,juce::Justification::centredLeft);
        g.setColour(ink.withAlpha(0.16f));g.drawHorizontalLine(215,28,float(getWidth()-28));
        g.setColour(ink);g.setFont(font(21));
        g.drawText("Old tail fades while the new effect plays.",28,315,getWidth()-56,30,juce::Justification::centredLeft);
        g.drawText("Transitions temporarily increase CPU use.",28,345,getWidth()-56,30,juce::Justification::centredLeft);
    }
    void resized() override {
        low_latency_.setBounds(28,75,getWidth()-56,64);
        spillover_.setBounds(28,235,245,64);duration_.setBounds(280,243,170,48);
    }
private:
    juce::ToggleButton low_latency_,spillover_;
    juce::ComboBox duration_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment_,spillover_attachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> duration_attachment_;
};

class QuickPresetKey final : public juce::Button {
public:
    QuickPresetKey():Button("Quick preset") {}
    void setImage(const juce::Image& image) {cap_=image;}
    void update(unsigned slot,const juce::String& preset,bool active,bool available) {
        const bool changed=slot_!=slot || preset_!=preset || active_!=active;
        slot_=slot;preset_=preset;active_=active;
        setEnabled(available && preset.isNotEmpty());setToggleState(active,juce::dontSendNotification);
        setTitle("Quick preset "+juce::String(slot+1)+": "+(preset.isEmpty()?"Empty":preset));
        setTooltip(preset.isEmpty()?"Assign a preset to key "+juce::String(slot+1)+" in the preset bank.":"Load "+preset);
        if(changed) repaint();
    }
    void paintButton(juce::Graphics& g,bool over,bool down) override {
        const float centre=getWidth()*0.5f;
        g.setColour(juce::Colour(0xff282523));g.fillEllipse(centre-7,4,14,14);
        g.setColour(active_?juce::Colour(0xffed3226):juce::Colour(0xff5e2e28));g.fillEllipse(centre-4.5f,6.5f,9,9);
        if(active_) {g.setColour(juce::Colour(0xffffb498));g.fillEllipse(centre-2,7.5f,3,3);}
        const juce::Rectangle<float> cap(centre-62,30+(down?3.0f:0.0f),124,124-(down?3.0f:0.0f));
        g.drawImage(cap_,cap,juce::RectanglePlacement::centred);
        g.setColour(ink.withAlpha(preset_.isEmpty()?0.45f:1.0f));g.setFont(font(35,true));
        g.drawText(juce::String(slot_+1),cap.translated(0,-8),juce::Justification::centred);
        g.setFont(font(18));g.setColour(ink);
        g.drawFittedText(preset_.isEmpty()?"Empty":preset_,0,162,getWidth(),28,juce::Justification::centredTop,1,0.8f);
    }
private:
    unsigned slot_=0;
    juce::String preset_;
    juce::Image cap_;
    bool active_=false;
};

class PresetDialog final : public juce::AlertWindow {
public:
    PresetDialog(const juce::String& title,const juce::String& message,juce::Component& owner)
        :AlertWindow(title,message,juce::MessageBoxIconType::NoIcon,&owner),owner_(&owner) {}
    void addButton(const juce::String& name,int result,const juce::KeyPress& key) {
        AlertWindow::addButton(name,result,key);
        getButton(getNumButtons()-1)->onClick=[safe=juce::Component::SafePointer<PresetDialog>(this),result] {
            if(safe) safe->finish(result);
        };
    }
    void finish(int result) {
        auto callback=std::move(onResult);
        setVisible(false);exitModalState(0);
        // Finish editor state synchronously. A host may defer JUCE's modal
        // completion message after hiding a window; it only owns deletion now.
        if(callback) callback(result);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if(key==juce::KeyPress::escapeKey) {finish(0);return true;}
        return AlertWindow::keyPressed(key);
    }
    bool canModalEventBeSentToComponent(const juce::Component* component) override {
        // A hidden editor must never leave a process-wide input lock behind.
        return !owner_ || !isShowing() || (component!=owner_ && !owner_->isParentOf(component));
    }
    std::function<void(int)> onResult;
private:
    juce::Component::SafePointer<juce::Component> owner_;
};

class PresetBrowser final : public juce::Component {
public:
    explicit PresetBrowser(NativeHallProcessor& processor)
        :processor_(processor),filter_model_(*this),preset_model_(*this),filters_("Algorithms",&filter_model_),presets_("Presets",&preset_model_) {
        setComponentID("preset_browser");setName("Preset bank");setWantsKeyboardFocus(true);
        search_.setComponentID("preset_search");search_.setFont(font(22));
        search_.setTextToShowWhenEmpty("Search presets or algorithms",led_text.withAlpha(0.6f));
        search_.setTooltip("Search the preset name, algorithm or model.");
        search_.onTextChange=[this]{rebuild();};
        search_.onReturnKey=[this]{loadRow(presets_.getSelectedRow(),false);};
        search_.onEscapeKey=[this]{dismiss();};
        filters_.setComponentID("preset_algorithm_filter");presets_.setComponentID("preset_list");
        for(auto* list:{&filters_,&presets_}) {
            list->setRowHeight(38);list->setOutlineThickness(1);
            list->setColour(juce::ListBox::backgroundColourId,juce::Colours::transparentBlack);
            list->setColour(juce::ListBox::outlineColourId,ink.withAlpha(0.25f));addAndMakeVisible(*list);
        }
        all_.setComponentID("preset_all_algorithms");all_.setButtonText("All algorithms");
        all_.onClick=[this]{selected_.fill(false);filters_.repaint();rebuild();};
        close_.setComponentID("close_preset_browser");close_.setButtonText("Close");
        close_.onClick=[this]{dismiss();};
        load_.setComponentID("load_browser_preset");load_.setButtonText("Load preset");
        load_.onClick=[this]{loadRow(presets_.getSelectedRow(),false);};
        save_.setComponentID("save_browser_preset");save_.setButtonText("Save preset...");
        save_.onClick=[this]{dismiss();if(onSave) onSave();};
        assign_.setComponentID("assign_quick_preset");assign_.setButtonText("Assign to...");
        assign_.setTooltip("Assign the selected preset to one of the eight quick keys. Replaces that key's previous assignment.");
        assign_.onClick=[this]{showAssignments();};
        for(auto* button:{&all_,&close_,&load_,&save_,&assign_}) addAndMakeVisible(*button);
        addAndMakeVisible(search_);
        setVisible(false);
    }
    juce::Result open() {
        juce::Array<NativeHallProcessor::PresetInfo> entries;
        auto result=processor_.presetBankEntries(entries);if(result.failed()) return result;
        visible_.clear();presets_.deselectAllRows();
        entries_=std::move(entries);
        status_.clear();filters_.updateContent();rebuild();
        setVisible(true);toFront(false);search_.grabKeyboardFocus();return result;
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black.withAlpha(0.52f));
        auto& look=static_cast<InstrumentLook&>(getLookAndFeel());
        const auto bounds=body();look.drawMetalSurface(g,bounds.toFloat());
        look.drawScreenHeader(g,"Preset bank",{float(bounds.getX()+18),float(bounds.getY()+18),float(bounds.getWidth()-36),48},26);
        g.setColour(ink);g.setFont(font(18,true));
        g.drawText("ALGORITHM FILTER",filters_.getX(),filters_.getY()-28,filters_.getWidth(),24,juce::Justification::centredLeft);
        const int split=presets_.getWidth()/2;
        g.drawText("PRESET",presets_.getX()+12,presets_.getY()-28,split-24,24,juce::Justification::centredLeft);
        g.drawText("ALGORITHM / MODEL",presets_.getX()+split+12,presets_.getY()-28,split-24,24,juce::Justification::centredLeft);
        g.setFont(font(18));
        g.drawText(status_.isEmpty()?juce::String(visible_.size())+" presets · Double-click or Enter to load":status_,
            bounds.getX()+20,bounds.getBottom()-91,bounds.getWidth()-40,28,juce::Justification::centredLeft);
        if(visible_.isEmpty()) {
            g.setFont(font(22));g.drawText(entries_.isEmpty()?"No saved presets yet":"No presets match these filters",
                presets_.getBounds().reduced(20),juce::Justification::centred);
        }
    }
    void resized() override {
        const auto bounds=body();const int left=bounds.getX()+20,top=bounds.getY()+85;
        all_.setBounds(left,top,292,36);search_.setBounds(left+312,top,bounds.getWidth()-352,36);
        filters_.setBounds(left,top+72,292,bounds.getHeight()-255);
        presets_.setBounds(left+312,top+72,bounds.getWidth()-352,bounds.getHeight()-255);
        save_.setBounds(left,bounds.getBottom()-53,188,34);
        assign_.setBounds(bounds.getRight()-530,bounds.getBottom()-53,170,34);
        load_.setBounds(bounds.getRight()-340,bounds.getBottom()-53,170,34);
        close_.setBounds(bounds.getRight()-150,bounds.getBottom()-53,130,34);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if(key==juce::KeyPress::escapeKey) {dismiss();return true;}
        if(key==juce::KeyPress::returnKey) {loadRow(presets_.getSelectedRow(),false);return true;}
        return false;
    }
    std::function<void()> onClose,onSave,onLoad,onAssign;
private:
    juce::Rectangle<int> body() const {return {230,175,1180,690};}
    juce::String algorithmLabel(const NativeHallProcessor::PresetInfo& entry) const {
        const auto* firmware=cineol::find_firmware(entry.firmware.toStdString());
        const auto model=entry.firmware=="224-v4.4"?juce::String("224"):entry.firmware=="224xl-v8.21"?juce::String("224 XL"):
            firmware?juce::String(firmware->name.data()):entry.firmware;
        return processor_.getProgramName(entry.algorithm)+" · "+model;
    }
    bool available(const NativeHallProcessor::PresetInfo& entry) const {
        const auto* firmware=cineol::find_firmware(entry.firmware.toStdString());
        return firmware && firmware->selectable && processor_.programAvailable(entry.algorithm);
    }
    void rebuild() {
        const auto row=presets_.getSelectedRow();
        const auto previous=juce::isPositiveAndBelow(row,visible_.size())?entries_[visible_[row]].name:processor_.presetName();
        const auto query=search_.getText().trim();
        const bool filtered=std::any_of(selected_.begin(),selected_.end(),[](bool value){return value;});
        visible_.clear();int selected_row=-1;
        for(int i=0;i<entries_.size();++i) {
            const auto& entry=entries_[i];
            if(filtered && !selected_[size_t(entry.algorithm)]) continue;
            if(query.isNotEmpty() && !entry.name.containsIgnoreCase(query) && !algorithmLabel(entry).containsIgnoreCase(query)) continue;
            if(entry.name.equalsIgnoreCase(previous)) selected_row=visible_.size();visible_.add(i);
        }
        presets_.deselectAllRows();presets_.updateContent();
        if(selected_row>=0) presets_.selectRow(selected_row);
        all_.setToggleState(!filtered,juce::dontSendNotification);updateLoad();repaint();
    }
    void updateLoad() {
        const int row=presets_.getSelectedRow();
        load_.setEnabled(juce::isPositiveAndBelow(row,visible_.size()) && available(entries_[visible_[row]]));
        assign_.setEnabled(juce::isPositiveAndBelow(row,visible_.size()));
    }
    void showAssignments() {
        const auto row=presets_.getSelectedRow();
        if(!juce::isPositiveAndBelow(row,visible_.size())) return;
        const auto selected=entries_[visible_[row]].name;
        const auto names=processor_.quickPresetNames();
        juce::PopupMenu menu;menu.setLookAndFeel(&getLookAndFeel());
        for(unsigned slot=0;slot<names.size();++slot)
            menu.addItem(int(slot+1),juce::String(slot+1)+"   "+(names[slot].isEmpty()?"Empty":names[slot]));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&assign_).withMinimumWidth(240),
            [safe=juce::Component::SafePointer<PresetBrowser>(this),selected](int choice) {
                if(!safe || choice<1 || choice>int(NativeHallProcessor::quick_preset_count)) return;
                const auto result=safe->processor_.assignQuickPreset(unsigned(choice-1),selected);
                safe->status_=result.wasOk()?selected+" assigned to key "+juce::String(choice):result.getErrorMessage();
                if(result.wasOk() && safe->onAssign) safe->onAssign();
                safe->repaint();
            });
    }
    void toggleFilter(int row) {
        if(!juce::isPositiveAndBelow(row,int(selected_.size()))) return;
        selected_[size_t(row)]=!selected_[size_t(row)];status_.clear();filters_.repaint();rebuild();
    }
    void loadRow(int row,bool close) {
        if(!juce::isPositiveAndBelow(row,visible_.size())) return;
        const auto result=processor_.loadBankPreset(entries_[visible_[row]].name);
        if(result.failed()) {status_=result.getErrorMessage();repaint();return;}
        status_.clear();if(onLoad) onLoad();presets_.repaint();repaint();if(close) dismiss();
    }
    void dismiss() {setVisible(false);if(onClose) onClose();}
    struct FilterModel final : juce::ListBoxModel {
        explicit FilterModel(PresetBrowser& browser):owner(browser) {}
        int getNumRows() override {return owner.processor_.getNumPrograms();}
        void paintListBoxItem(int row,juce::Graphics& g,int width,int height,bool) override {
            if(!juce::isPositiveAndBelow(row,getNumRows())) return;
            const bool selected=owner.selected_[size_t(row)];
            if(selected) {g.setColour(juce::Colour(0xff6d2c28));g.fillRect(0,0,width,height);}
            g.setColour(selected?juce::Colours::white:ink);g.drawRect(10,height/2-7,14,14,1);
            if(selected) {juce::Path tick;tick.startNewSubPath(12,float(height/2));tick.lineTo(16,float(height/2+4));tick.lineTo(22,float(height/2-4));g.strokePath(tick,juce::PathStrokeType(2));}
            g.setFont(font(18));g.drawText(owner.processor_.getProgramName(row),34,0,width-91,height,juce::Justification::centredLeft);
            g.setFont(font(15));g.drawText(NativeHallProcessor::isXL(row)?"XL":"224",width-48,0,40,height,juce::Justification::centredRight);
        }
        void listBoxItemClicked(int row,const juce::MouseEvent&) override {owner.toggleFilter(row);}
        void returnKeyPressed(int row) override {owner.toggleFilter(row);}
        juce::String getTooltipForRow(int row) override {return owner.processor_.getProgramName(row)+": click to include or remove this algorithm. Multiple selections are allowed.";}
        PresetBrowser& owner;
    };
    struct PresetModel final : juce::ListBoxModel {
        explicit PresetModel(PresetBrowser& browser):owner(browser) {}
        int getNumRows() override {return owner.visible_.size();}
        void paintListBoxItem(int row,juce::Graphics& g,int width,int height,bool selected) override {
            if(!juce::isPositiveAndBelow(row,getNumRows())) return;
            const auto& entry=owner.entries_[owner.visible_[row]];
            if(selected) {g.setColour(juce::Colour(0xff6d2c28));g.fillRect(0,0,width,height);}
            g.setColour((selected?juce::Colours::white:ink).withAlpha(owner.available(entry)?1.0f:0.45f));
            g.setFont(font(20));g.drawText(entry.name,12,0,width/2-24,height,juce::Justification::centredLeft);
            g.setFont(font(18));g.drawText(owner.algorithmLabel(entry),width/2+12,0,width/2-24,height,juce::Justification::centredLeft);
            g.setColour(ink.withAlpha(0.1f));g.drawHorizontalLine(height-1,0,float(width));
        }
        void selectedRowsChanged(int) override {owner.updateLoad();}
        void listBoxItemDoubleClicked(int row,const juce::MouseEvent&) override {owner.loadRow(row,true);}
        void returnKeyPressed(int row) override {owner.loadRow(row,false);}
        juce::String getTooltipForRow(int row) override {
            if(!juce::isPositiveAndBelow(row,getNumRows())) return {};
            const auto& entry=owner.entries_[owner.visible_[row]];
            return entry.name+"\n"+owner.algorithmLabel(entry)+(owner.available(entry)?"":"\nImport the required ROMs to load this preset.");
        }
        PresetBrowser& owner;
    };
    NativeHallProcessor& processor_;
    juce::Array<NativeHallProcessor::PresetInfo> entries_;
    juce::Array<int> visible_;
    std::array<bool,NativeHallProcessor::program_count> selected_{};
    FilterModel filter_model_;PresetModel preset_model_;
    juce::ListBox filters_,presets_;
    juce::TextEditor search_;
    juce::TextButton all_,close_,load_,save_,assign_;
    juce::String status_;
};

class RomSetup final : public juce::Component,private juce::Timer {
public:
    explicit RomSetup(NativeHallProcessor& processor):processor_(processor),progress_bar_(progress_) {
        setLookAndFeel(&look_);
        setComponentID("rom_setup");setName("Import Lexicon 224 or 224 XL ROMs");
        title_.setText("Load your Lexicon ROMs",juce::dontSendNotification);
        title_.setFont(font(38,true));title_.setJustificationType(juce::Justification::centred);
        instructions_.setText("224 v4.4: 5 files / 224 XL v8.21: 11 files\nSelect a folder, ZIP, or all ROM files together.\nOther firmware versions are not supported.",juce::dontSendNotification);
        instructions_.setFont(font(25));instructions_.setJustificationType(juce::Justification::centred);
        status_.setComponentID("rom_status");status_.setFont(font(22));
        status_.setJustificationType(juce::Justification::centred);
        for(auto* label:{&title_,&instructions_,&status_}) {
            label->setColour(juce::Label::textColourId,label==&title_?led_text:ink);addAndMakeVisible(*label);
        }
        choose_.setComponentID("choose_roms");choose_.setButtonText("Choose ROMs...");
        choose_.setColour(juce::TextButton::buttonColourId,ink);
        choose_.setColour(juce::TextButton::textColourOffId,juce::Colours::white);
        choose_.setTooltip("Supported ROM sets: original 224 v4.4 and 224 XL v8.21.");
        choose_.onClick=[this]{chooseRoms();};addAndMakeVisible(choose_);
        folder_.setComponentID("choose_rom_folder");folder_.setButtonText("Choose folder...");
        folder_.setColour(juce::TextButton::buttonColourId,ink);
        folder_.setColour(juce::TextButton::textColourOffId,juce::Colours::white);
        folder_.onClick=[this]{chooseRoms(true);};addAndMakeVisible(folder_);
        cancel_.setComponentID("cancel_rom_import");cancel_.setButtonText("Cancel import");
        cancel_.setColour(juce::TextButton::buttonColourId,ink);
        cancel_.setColour(juce::TextButton::textColourOffId,juce::Colours::white);
        cancel_.onClick=[this]{processor_.cancelRomImport();};addAndMakeVisible(cancel_);
        close_.setComponentID("close_rom_setup");close_.setButtonText("Close");
        close_.setColour(juce::TextButton::buttonColourId,ink);
        close_.setColour(juce::TextButton::textColourOffId,juce::Colours::white);
        close_.onClick=[this]{requested_=false;setVisible(shouldBeVisible());};addAndMakeVisible(close_);
        addAndMakeVisible(progress_bar_);update();startTimerHz(10);
    }
    ~RomSetup() override {stopTimer();setLookAndFeel(nullptr);}
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black.withAlpha(0.64f));
        look_.drawMetalSurface(g,{245,285,980,500});
        look_.drawScreenHeader(g,{}, {285,315,900,60},30);
    }
    void resized() override {
        title_.setBounds(285,315,900,60);instructions_.setBounds(285,390,900,115);
        status_.setBounds(290,520,890,95);progress_bar_.setBounds(365,625,740,25);
        choose_.setBounds(320,680,330,62);folder_.setBounds(675,680,330,62);
        cancel_.setBounds(410,680,440,62);close_.setBounds(1030,680,160,62);
    }
    bool shouldBeVisible() const {return requested_ || processor_.importingRoms() || (!processor_.programAvailable(0) && !processor_.programAvailable(6));}
    void open() {
        requested_=true;setVisible(true);toFront(false);update();
    }
private:
    struct SetupLook final : InstrumentLook {
        void drawButtonText(juce::Graphics& g,juce::TextButton& button,bool,bool) override {
            g.setColour(button.findColour(juce::TextButton::textColourOffId).withAlpha(button.isEnabled()?1.0f:0.5f));
            g.setFont(font(25,true));
            g.drawText(button.getButtonText(),button.getLocalBounds(),juce::Justification::centred);
        }
    } look_;
    void chooseRoms(bool folder=false) {
        requested_=true;
        chooser_open_=true;
        chooser_=std::make_unique<juce::FileChooser>("Choose Lexicon 224 v4.4 or 224 XL v8.21 ROMs",juce::File{},"*");
        choose_.setEnabled(false);
        // Windows native dialogs select either files or folders. Combining
        // both flags silently turns the picker into a folders-only dialog.
        const int selection=folder?juce::FileBrowserComponent::canSelectDirectories:
            juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::canSelectMultipleItems;
        chooser_->launchAsync(juce::FileBrowserComponent::openMode|selection,
            [safe=juce::Component::SafePointer<RomSetup>(this)](const juce::FileChooser& chooser) {
                if(!safe) return;
                safe->chooser_open_=false;
                const auto files=chooser.getResults();
                if(!files.isEmpty()) safe->processor_.importRoms(files);
                safe->update();
            });
    }
    void update() {
        const bool busy=processor_.importingRoms();progress_=processor_.importProgress();
        const bool original_ready=processor_.programAvailable(0),xl_ready=processor_.programAvailable(6);
        const auto status=processor_.romStatus();
        if(!busy) {
            if(status.endsWith("ready.") && ((original_ready && !original_ready_) || (xl_ready && !xl_ready_))) requested_=false;
            original_ready_=original_ready;xl_ready_=xl_ready;
        }
        status_.setText(status,juce::dontSendNotification);
        choose_.setVisible(!busy);choose_.setEnabled(!busy && !chooser_open_);
        folder_.setVisible(!busy);folder_.setEnabled(!busy && !chooser_open_);
        cancel_.setVisible(busy);progress_bar_.setVisible(busy);
        close_.setEnabled((original_ready || xl_ready) && !busy && !chooser_open_);
    }
    void timerCallback() override {update();}
    NativeHallProcessor& processor_;
    juce::Label title_,instructions_,status_;
    juce::TextButton choose_,folder_,cancel_,close_;
    double progress_=0;
    juce::ProgressBar progress_bar_;
    std::unique_ptr<juce::FileChooser> chooser_;
    bool chooser_open_=false;
    bool requested_=false,original_ready_=false,xl_ready_=false;
};
}

struct CineolEditor::Panel final : public juce::Component,private juce::Timer,private juce::FocusChangeListener {
    explicit Panel(NativeHallProcessor& processor):processor_(processor),background_(loadImage("panelquickpresets_png")),bezel_(loadImage("panel_png")),rom_setup_(processor),settings_panel_(processor),preset_browser_(processor) {
        setComponentID("cineol_panel");setLookAndFeel(&look_);setSize(panel_width,panel_height);addAndMakeVisible(display_);
        display_.setBounds(variable_fader_x,100,1512,240);
        display_.algorithm.onClick=[this]{showAlgorithms();};
        page_=processor_.editorPage();
        for(unsigned i=0;i<display_.page_buttons.size();++i) display_.page_buttons[i].onClick=[this,i]{changePage(int(i));};
        display_.firmware.onClick=[this]{rom_setup_.open();};
        display_.preset.onClick=[this]{showPresets();};
        display_.save.onClick=[this]{namePreset();};
        for(unsigned i=0;i<sliders_.size();++i) {
            auto& slider=sliders_[i];auto* param=processor_.state.getParameter(NativeHallProcessor::ids[i]);
            slider.setLookAndFeel(&look_);
            slider.setColour(juce::Slider::textBoxTextColourId,ink);
            slider.setColour(juce::Slider::textBoxBackgroundColourId,juce::Colours::transparentBlack);
            slider.setColour(juce::Slider::textBoxOutlineColourId,juce::Colours::transparentBlack);
            slider.setComponentID(NativeHallProcessor::ids[i]);slider.setName(param->getName(40));
            slider.setTitle(param->getName(40));slider.setSliderStyle(juce::Slider::LinearVertical);
            slider.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
            slider.setTextValueSuffix(param->getLabel().isEmpty()?juce::String{}:" "+param->getLabel());
            slider.setSliderSnapsToMousePosition(false);
            slider.setTooltip(param->getName(40)+": drag or use arrow keys. Double-click resets.");
            addAndMakeVisible(slider);
            const int slot=i<6?int(i):i==6?4:int(i);
            slider.setBounds(i<7?variable_fader_x+slot*fader_step:global_fader_x+int(i-6)*global_fader_step,fader_y,140,fader_height);
            slider_attachments_[i]=std::make_unique<SliderAttachment>(processor_.state,NativeHallProcessor::ids[i],slider);
            slider.connect(processor_);
            slider.onDragStart=[this,i]{focused_=i;refreshDisplay();};
            slider.onValueChange=[this,i]{focused_=i;refreshDisplay();};
        }
        for(unsigned slot=0;slot<xl_sliders_.size();++slot) {
            auto& slider=xl_sliders_[slot];
            slider.setLookAndFeel(&look_);slider.setComponentID("xl_slot_"+juce::String(slot+1));
            slider.setSliderStyle(juce::Slider::LinearVertical);slider.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
            slider.setSliderSnapsToMousePosition(false);slider.setBounds(variable_fader_x+int(slot)*fader_step,fader_y,140,fader_height);
            addChildComponent(slider);slider.connect(processor_);
            slider.onDragStart=[this,slot]{xl_dragging_[slot]=true;focused_xl_slot_=slot;if(xl_attachments_[slot]) xl_attachments_[slot]->beginGesture();};
            slider.onDragEnd=[this,slot]{xl_dragging_[slot]=false;if(xl_attachments_[slot]) xl_attachments_[slot]->endGesture();};
            slider.onValueChange=[this,slot]{
                if(!binding_xl_ && xl_attachments_[slot]) {
                    focused_xl_slot_=slot;
                    if(xl_dragging_[slot]) xl_attachments_[slot]->setValueAsPartOfGesture(float(xl_sliders_[slot].getValue()/xl_scales_[slot]));
                    else xl_attachments_[slot]->setValueAsCompleteGesture(float(xl_sliders_[slot].getValue()/xl_scales_[slot]));
                }
                refreshDisplay();
            };
        }
        for(unsigned slot=0;slot<parked_.size();++slot) {
            auto& slider=parked_[slot];slider.setComponentID("inactive_"+juce::String(slot+1));
            slider.setLookAndFeel(&look_);
            slider.setSliderStyle(juce::Slider::LinearVertical);slider.setRange(0,1);slider.setValue(0);
            slider.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);slider.setEnabled(false);
            slider.setBounds(variable_fader_x+int(slot)*fader_step,fader_y,140,fader_height);slider.setTitle("Inactive fader");
            addChildComponent(slider);
        }
        for(unsigned i=0;i<display_.outputs.size();++i) {
            auto& output=display_.outputs[i];const char* id=NativeHallProcessor::ids[12+i];
            output.setTitle(i==0?"Left Output":"Right Output");output.setTooltip("Choose output A, B, C or D.");
            output.onClick=[this,i]{showOutputs(i);};
            output_attachments_[i]=std::make_unique<juce::ParameterAttachment>(*processor_.state.getParameter(id),[this](float){refreshDisplay();});
            output_attachments_[i]->sendInitialUpdate();
        }
        for(unsigned i=0;i<display_.modes.size();++i) {
            auto& button=display_.modes[i];const char* id=NativeHallProcessor::ids[10+i];
            button.setTitle(processor_.state.getParameter(id)->getName(40));
            button_attachments_[i]=std::make_unique<ButtonAttachment>(processor_.state,id,button);
            button.onStateChange=[this]{refreshDisplay();};
        }
        dynamic_attachment_=std::make_unique<juce::ParameterAttachment>(*processor_.state.getParameter("xl_42"),
            [this](float){refreshDisplay();});
        display_.dynamic.onClick=[this] {
            // Keep the existing logical control ID and every DAW parameter
            // index. The dynamic bit already belongs to sound presets.
            const uint8_t raw=processor_.xlControl(42);
            dynamic_attachment_->setValueAsCompleteGesture(float(raw^1));refreshDisplay();
        };
        dirt_.setLookAndFeel(&look_);dirt_.setComponentID("dirt");dirt_.setName("Dirt");dirt_.setTitle("Dirt");
        dirt_.setSliderStyle(juce::Slider::LinearVertical);dirt_.setRange(0,1,0.001);dirt_.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
        dirt_.setSliderSnapsToMousePosition(false);dirt_.setDoubleClickReturnValue(true,0);dirt_.connect(processor_);
        dirt_.setBounds(global_fader_x,fader_y,140,fader_height);addAndMakeVisible(dirt_);
        dirt_.setTooltip("Dirt: 0% filtered; 100% digital bypass. The lower half introduces artifacts gently.");
        dirt_attachment_=std::make_unique<juce::ParameterAttachment>(*processor_.state.getParameter("analog"),
            [this](float clean){
                const juce::ScopedValueSetter<bool> binding(binding_dirt_,true);
                dirt_.setValue(1-clean,juce::sendNotificationSync);refreshDisplay();
            });
        dirt_.onDragStart=[this]{dragging_dirt_=true;dirt_attachment_->beginGesture();};
        dirt_.onDragEnd=[this]{dragging_dirt_=false;dirt_attachment_->endGesture();};
        dirt_.onValueChange=[this]{
            if(!binding_dirt_) {
                const float clean=float(1-dirt_.getValue());
                if(dragging_dirt_) dirt_attachment_->setValueAsPartOfGesture(clean);
                else dirt_attachment_->setValueAsCompleteGesture(clean);
            }
            refreshDisplay();
        };
        dirt_attachment_->sendInitialUpdate();
        algorithm_attachment_=std::make_unique<juce::ParameterAttachment>(*processor_.state.getParameter("algorithm"),[this](float){refreshProgram();});
        algorithm_attachment_->sendInitialUpdate();
        const auto quick_cap=trimTransparentPadding(loadImage("quickkey_png"));
        for(unsigned slot=0;slot<quick_keys_.size();++slot) {
            auto& key=quick_keys_[slot];key.setComponentID("quick_preset_"+juce::String(slot+1));
            key.setImage(quick_cap);
            key.setBounds(100+int(slot)*180,950,180,190);addAndMakeVisible(key);
            key.onClick=[this,slot]{reportPresetResult(processor_.loadQuickPreset(slot));};
        }
        addChildComponent(rom_setup_);rom_setup_.setBounds(0,0,panel_width,panel_height);
        addChildComponent(settings_panel_);settings_panel_.setBounds(990,76,550,400);
        addChildComponent(preset_browser_);preset_browser_.setBounds(0,0,panel_width,panel_height);
        preset_browser_.onClose=[this]{display_.preset.grabKeyboardFocus();};
        preset_browser_.onSave=[this]{namePreset();};preset_browser_.onLoad=[this]{refreshProgram();};
        preset_browser_.onAssign=[this]{refreshProgram();};
        addAndMakeVisible(settings_button_);settings_button_.setBounds(1480,12,64,52);
        settings_button_.onClick=[this] {
            settings_panel_.setVisible(settings_button_.getToggleState());
            if(settings_panel_.isVisible()) {settings_panel_.toFront(false);settings_panel_.grabKeyboardFocus();}
        };
        refreshProgram();juce::Desktop::getInstance().addFocusChangeListener(this);startTimerHz(30);
    }
    ~Panel() override {
        stopTimer();juce::Desktop::getInstance().removeFocusChangeListener(this);cancelTransientUi();
        for(auto dialog:owned_dialogs_) dialog.deleteAndZero();
        setLookAndFeel(nullptr);
    }
    void paint(juce::Graphics& g) override {
        // One full-size photographic faceplate: no stretched strips or joins.
        g.drawImage(background_,getLocalBounds().toFloat(),juce::RectanglePlacement::stretchToFit);
        drawBezel(g,bezel_,{60,65,1354,178},{52,78,1536,274},12);
        g.setColour(ink);g.setFont(font(40,true));g.drawText("Cineol-X 224",84,17,800,44,juce::Justification::centredLeft);
        g.setColour(ink.withAlpha(0.45f));g.drawVerticalLine(global_fader_x-30,370,883.0f);
        g.setFont(font(21,true));g.setColour(ink);g.drawText("QUICK PRESETS",600,916,440,28,juce::Justification::centred);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if(key==juce::KeyPress::escapeKey && settings_panel_.isVisible()) {
            settings_panel_.setVisible(false);settings_button_.setToggleState(false,juce::dontSendNotification);
            settings_button_.grabKeyboardFocus();return true;
        }
        return false;
    }
private:
    using SliderAttachment=juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment=juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment=juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    MotorFader& slotFader(unsigned slot) {
        if(displayed_xl_) return xl_sliders_[slot];
        if(page_==0) return sliders_[slot];
        return slot==4?sliders_[6]:parked_[slot];
    }
    void changePage(int next) {
        if(next==page_) return;
        std::array<double,6> positions{};
        for(unsigned slot=0;slot<6;++slot) positions[slot]=slotFader(slot).visualProportion();
        page_=next;processor_.setEditorPage(page_);
        refreshProgram();
        for(unsigned slot=0;slot<6;++slot) slotFader(slot).animateFrom(positions[slot]);
    }
    void showAlgorithms() {
        juce::PopupMenu menu;menu.setLookAndFeel(&look_);
        for(int program=0;program<processor_.getNumPrograms();++program) {
            if(program==0) menu.addSectionHeader("224");
            if(program==6) {menu.addSeparator();menu.addSectionHeader("224 XL");}
            menu.addItem(program+1,processor_.getProgramName(program),processor_.programAvailable(program),program==processor_.getCurrentProgram());
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&display_.algorithm).withMinimumWidth(310).withStandardItemHeight(30).withMaximumNumColumns(1),
            [safe=juce::Component::SafePointer<Panel>(this)](int item){if(safe && item>0) safe->processor_.setCurrentProgram(item-1);});
    }
    void showOutputs(unsigned channel) {
        juce::PopupMenu menu;menu.setLookAndFeel(&look_);auto* parameter=processor_.state.getParameter(NativeHallProcessor::ids[12+channel]);
        menu.addSectionHeader(channel==0?"Left output":"Right output");
        const int selected=int(parameter->convertFrom0to1(parameter->getValue()));
        for(int i=0;i<4;++i) menu.addItem(i+1,juce::String::charToString(char('A'+i)),true,i==selected);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&display_.outputs[channel]).withMinimumWidth(210).withStandardItemHeight(30),
            [safe=juce::Component::SafePointer<Panel>(this),channel](int item){if(safe && item>0) safe->output_attachments_[channel]->setValueAsCompleteGesture(float(item-1));});
    }
    void showPresets() {
        const auto result=preset_browser_.open();if(result.failed()) reportPresetResult(result);
    }
    void reportPresetResult(const juce::Result& result) {
        if(result.failed()) showPresetDialog("Preset",result.getErrorMessage(),"OK",{},{});
        refreshProgram();
    }
    void showPresetDialog(const juce::String& title,const juce::String& message,const juce::String& accept,
                          const juce::String& cancel,std::function<void(int)> callback) {
        if(preset_dialog_open_) return;
        preset_dialog_open_=true;refreshProgram();
        auto* owner=findParentComponentOfClass<juce::AudioProcessorEditor>();
        jassert(owner!=nullptr);if(!owner) {preset_dialog_open_=false;return;}
        auto* dialog=new PresetDialog(title,message,*owner);
        dialog->setLookAndFeel(&look_);
        dialog->addButton(accept,1,juce::KeyPress(juce::KeyPress::returnKey));
        if(cancel.isNotEmpty()) dialog->addButton(cancel,0,juce::KeyPress(juce::KeyPress::escapeKey));
        preset_dialog_=dialog;
        owned_dialogs_.emplace_back(dialog);
        owner->addAndMakeVisible(dialog);dialog->centreWithSize(dialog->getWidth(),dialog->getHeight());
        dialog->onResult=
            [safe=juce::Component::SafePointer<Panel>(this),callback=std::move(callback),window=juce::Component::SafePointer<juce::AlertWindow>(dialog)](int answer) {
                if(window) window->setLookAndFeel(nullptr);
                if(!safe || safe->preset_dialog_!=window.getComponent()) return;
                safe->preset_dialog_open_=false;safe->preset_dialog_=nullptr;
                if(callback) callback(answer);
                safe->refreshProgram();
            };
        dialog->enterModalState(true,nullptr,true);
    }
    void storePreset(const juce::String& name) {
        juce::StringArray names;auto result=processor_.presetNames(names);
        if(result.failed()) {reportPresetResult(result);return;}
        if(!names.contains(name.trim(),true)) {reportPresetResult(processor_.saveBankPreset(name));return;}
        showPresetDialog("Replace preset?","Replace \""+name.trim()+"\" in the preset bank?","Replace","Cancel",
            [safe=juce::Component::SafePointer<Panel>(this),name](int answer) {
                if(!safe) return;
                if(answer) safe->reportPresetResult(safe->processor_.saveBankPreset(name,true));
                else safe->refreshProgram();
            });
    }
    void namePreset() {
        if(preset_dialog_open_) return;
        preset_dialog_open_=true;refreshProgram();
        auto* owner=findParentComponentOfClass<juce::AudioProcessorEditor>();
        jassert(owner!=nullptr);if(!owner) {preset_dialog_open_=false;return;}
        auto* dialog=new PresetDialog("Save preset",
            "Algorithm: "+processor_.getProgramName(processor_.getCurrentProgram())+"\nName this preset to add it to the bank.",
            *owner);
        dialog->setLookAndFeel(&look_);
        const auto current=processor_.presetName();
        dialog->addTextEditor("preset_name",current=="Unsaved settings"?juce::String{}:current,"Preset name");
        dialog->getTextEditor("preset_name")->setInputRestrictions(80);
        dialog->addButton("Save",1,juce::KeyPress(juce::KeyPress::returnKey));
        dialog->addButton("Cancel",0,juce::KeyPress(juce::KeyPress::escapeKey));
        preset_dialog_=dialog;
        owned_dialogs_.emplace_back(dialog);
        owner->addAndMakeVisible(dialog);dialog->centreWithSize(dialog->getWidth(),dialog->getHeight());
        dialog->onResult=
            [safe=juce::Component::SafePointer<Panel>(this),window=juce::Component::SafePointer<juce::AlertWindow>(dialog)](int answer) {
                const auto name=window?window->getTextEditorContents("preset_name"):juce::String{};
                if(window) window->setLookAndFeel(nullptr);
                if(!safe || safe->preset_dialog_!=window.getComponent()) return;
                safe->preset_dialog_open_=false;safe->preset_dialog_=nullptr;
                if(answer && window) safe->storePreset(name);
                else safe->refreshProgram();
            };
        dialog->enterModalState(true,nullptr,true);
        dialog->getTextEditor("preset_name")->grabKeyboardFocus();
        dialog->getTextEditor("preset_name")->selectAll();
    }
    void bindXL() {
        const auto* data=processor_.xlProgram();binding_xl_=true;
        for(unsigned slot=0;slot<6;++slot) {
            xl_attachments_[slot].reset();auto& slider=xl_sliders_[slot];
            const unsigned cell=data?data->pages[unsigned(page_)].cells[slot]:255;
            const bool active=cell<48 && data->control_active(cell);
            slider.setParked(!active);slider.setEnabled(active && processor_.ready());
            if(!active) {slider.setRange(0,1);slider.setValue(0,juce::dontSendNotification);slider.setTitle("Inactive fader");continue;}
            const auto& page=data->pages[unsigned(page_)];
            slider.setName(juce::String(page.names[slot].data()));slider.setTitle(slider.getName());
            slider.setTooltip(slider.getName()+": drag or use arrow keys. Double-click restores the factory value.");
            unsigned parameter_index=NativeHallProcessor::xl_parameter_begin+cell;xl_scales_[slot]=1;
            if(data->chorus_page && data->pages[data->chorus_page-1].cells[data->chorus_slot]==cell) {parameter_index=15;xl_scales_[slot]=8;}
            if(data->diffusion_page && data->pages[data->diffusion_page-1].cells[data->diffusion_slot]==cell) {parameter_index=16;xl_scales_[slot]=4;}
            slider.getProperties().set("parameter_id",NativeHallProcessor::ids[parameter_index]);
            auto raw=data->controls.factory;for(unsigned i=0;i<raw.size();++i) raw[i]=processor_.xlControl(i);
            const unsigned maximum=page.variable_predelay[slot]?data->predelay_maximum(raw):page.maximum[slot];
            slider.setRange(0,std::max(xl_scales_[slot],maximum/xl_scales_[slot]*xl_scales_[slot]),xl_scales_[slot]);
            slider.setDoubleClickReturnValue(true,data->controls.factory[cell]);
            xl_attachments_[slot]=std::make_unique<juce::ParameterAttachment>(
                *processor_.state.getParameter(NativeHallProcessor::ids[parameter_index]),
                [this,slot,cell](float value){
                    const bool was_binding=binding_xl_;binding_xl_=true;
                    xl_sliders_[slot].setValue(value<0?processor_.xlControl(cell):value*xl_scales_[slot],juce::sendNotificationSync);
                    binding_xl_=was_binding;refreshDisplay();
                });
            xl_attachments_[slot]->sendInitialUpdate();
        }
        binding_xl_=false;
    }
    void synchronizeXL(const cineol::xl::ProgramData& data) {
        auto raw=data.controls.factory;
        for(unsigned cell=0;cell<raw.size();++cell) raw[cell]=processor_.xlControl(cell);
        data.resolve_controls(raw);binding_xl_=true;
        const auto& page=data.pages[unsigned(page_)];
        for(unsigned slot=0;slot<6;++slot) {
            const unsigned cell=page.cells[slot];if(cell>=48 || !data.control_active(cell)) continue;
            auto& slider=xl_sliders_[slot];const auto position=slider.visualProportion();
            const unsigned maximum=page.variable_predelay[slot]?data.predelay_maximum(raw):page.maximum[slot];
            const double range=std::max(xl_scales_[slot],maximum/xl_scales_[slot]*xl_scales_[slot]);
            const bool changed=range!=slider.getMaximum();
            if(changed) slider.setRange(0,range,xl_scales_[slot]);
            slider.setValue(raw[cell],juce::sendNotificationSync);
            if(changed) slider.animateFrom(position);
        }
        binding_xl_=false;
    }
    void refreshDisplay() {
        const int program=processor_.getCurrentProgram();
        const bool xl=processor_.usesXL();
        const unsigned focus=focused_;
        auto* focused=processor_.state.getParameter(NativeHallProcessor::ids[focus]);
        auto value=focused->getText(focused->getValue(),0);
        if(focused->getLabel().isNotEmpty()) value+=" "+focused->getLabel();
        if(!xl) display_.update(program,processor_.getProgramName(program),captions[focus],value);
        display_.firmware.setButtonText(xl?"224 XL":"224");
        display_.updatePreset(processor_.presetName(),processor_.presetModified());
        std::array<juce::String,9> names,values;
        for(unsigned slot=0;slot<names.size();++slot) {
            if(slot==6) {names[slot]="DIRT";values[slot]=juce::String((1-processor_.state.getParameter("analog")->getValue())*100,1)+" %";continue;}
            if(xl && slot<6) {
                const auto* data=processor_.xlProgram();const auto* page=data?&data->pages[unsigned(page_)]:nullptr;
                const unsigned cell=page?page->cells[slot]:255;
                if(cell>=48 || !data->control_active(cell)) {names[slot]="--";values[slot]="--";continue;}
                names[slot]=juce::String(page->names[slot].data());
                auto raw=data->controls.factory;
                for(unsigned i=0;i<raw.size();++i) raw[i]=processor_.xlControl(i);
                data->resolve_controls(raw);
                const auto text=data->display_value(unsigned(page_),slot,raw);
                const auto formatted=juce::String(text.data());
                // XL's formatter uses dashes for its unbounded upper values.
                // Keep that distinct from a parked/unavailable fader in the UI.
                values[slot]=formatted.startsWith("--")?"INF"+formatted.substring(2):formatted;continue;
            }
            const int index=slot>=7?int(slot):page_==0?int(slot):slot==4?6:-1;
            if(index<0 || (index==6 && program==3)) {names[slot]="--";values[slot]="--";continue;}
            auto* param=processor_.state.getParameter(NativeHallProcessor::ids[index]);
            names[slot]=index==15?"CHORUS":index==16?"DIFFUSION":captions[index];values[slot]=param->getText(param->getValue(),0);
            if(param->getLabel().isNotEmpty()) values[slot]+=" "+param->getLabel();
        }
        display_.updateSlots(names,values);
        if(xl) display_.update(program,processor_.getProgramName(program),names[focused_xl_slot_],values[focused_xl_slot_]);
        display_.updatePages(processor_.parameterPages(),unsigned(page_),processor_.ready());
        display_.getProperties().set("page_index",page_);
        for(unsigned i=0;i<2;++i) {
            const auto* parameter=processor_.state.getParameter(NativeHallProcessor::ids[12+i]);
            display_.outputs[i].setButtonText(juce::String(i==0?"LEFT ":"RIGHT ")+juce::String::charToString(char('A'+int(parameter->convertFrom0to1(parameter->getValue())))));
            const auto* data=processor_.xlProgram();
            const bool unavailable=xl && (!data || (i==0?!(data->modulation.flags&15):!data->dynamics.enabled));
            display_.modes[i].setButtonText(juce::String(i==0?"MOD ENH ":"DECAY OPT ")+(unavailable?juce::String("--"):display_.modes[i].getToggleState()?juce::String("ON"):juce::String("OFF")));
        }
        display_.dynamic.setToggleState((processor_.xlControl(42)&1)!=0,juce::dontSendNotification);
        const auto* dynamics_data=processor_.xlProgram();
        const bool dynamics_available=xl && dynamics_data && dynamics_data->dynamics.enabled;
        display_.dynamic.setButtonText(juce::String("DYN DECAY ")+(dynamics_available?(display_.dynamic.getToggleState()?"ON":"OFF"):"--"));
    }
    void refreshProgram() {
        const int program=processor_.getCurrentProgram();
        const bool ready=processor_.ready();const bool xl=processor_.usesXL();
        const bool engine_changed=displayed_program_>=0 && program!=displayed_program_;
        page_=std::clamp(page_,0,int(processor_.parameterPages())-1);
        const bool rebind=xl && (program!=bound_program_ || page_!=bound_page_ || processor_.ready()!=bound_ready_);
        std::array<double,6> positions{};
        if(engine_changed) for(unsigned slot=0;slot<6;++slot) positions[slot]=slotFader(slot).visualProportion();
        displayed_xl_=xl;
        rom_setup_.setVisible(rom_setup_.shouldBeVisible());
        display_.firmware.setTooltip(ready?"Engine used by the selected algorithm. Click to import ROMs.":processor_.romStatus()+" Click to import ROMs.");
        for(unsigned i=0;i<sliders_.size();++i) {
            auto& slider=sliders_[i];slider.setVisible(i>=7 || (!xl && (page_==0?i<6:i==6)));
            slider.setEnabled(ready && (i!=6 || program!=3));slider.setParked(i==6 && program==3);
        }
        for(auto& slider:xl_sliders_) slider.setVisible(xl);
        if(rebind) {bindXL();bound_program_=program;bound_page_=page_;bound_ready_=ready;}
        if(const auto* data=processor_.xlProgram()) synchronizeXL(*data);
        for(unsigned slot=0;slot<6;++slot) parked_[slot].setVisible(!xl && page_==1 && slot!=4);
        if(engine_changed) for(unsigned slot=0;slot<6;++slot) slotFader(slot).animateFrom(positions[slot]);
        // The unified list remains accessible so a missing bank cannot trap the UI.
        display_.algorithm.setEnabled(processor_.programAvailable(0) || processor_.programAvailable(6));
        display_.preset.setEnabled((processor_.programAvailable(0) || processor_.programAvailable(6)) && !preset_dialog_open_);
        display_.save.setEnabled(ready && !preset_dialog_open_);
        for(auto& output:display_.outputs) output.setEnabled(ready);
        dirt_.setEnabled(ready);
        const auto* data=processor_.xlProgram();
        for(unsigned i=0;i<display_.modes.size();++i) display_.modes[i].setEnabled(ready && (!xl || (data && (i==0?(data->modulation.flags&15):data->dynamics.enabled))));
        display_.modes[1].setTooltip("Decay Optimization: adapts feedback diffusion during the decay. Saved in presets.");
        display_.dynamic.setEnabled(ready && xl && data && data->dynamics.enabled);
        if(program!=displayed_program_) {
            displayed_program_=program;
            sliders_[6].setTooltip(program==3?"Acoustic Chamber does not use Diffusion.":
                "Diffusion: drag or use arrow keys. Double-click resets.");
            repaint();
        }
        refreshDisplay();refreshQuickPresets();
    }
    void refreshQuickPresets() {
        const auto names=processor_.quickPresetNames();const int active=processor_.activeQuickPreset();
        const bool available=processor_.programAvailable(0) || processor_.programAvailable(6);
        for(unsigned slot=0;slot<quick_keys_.size();++slot) quick_keys_[slot].update(slot,names[slot],active==int(slot),available && !preset_dialog_open_);
    }
    void cancelTransientUi() {
        if(auto* owner=findParentComponentOfClass<juce::AudioProcessorEditor>()) dismissEditorMenus(*owner);
        if(preset_dialog_) preset_dialog_->finish(0);
    }
    void globalFocusChanged(juce::Component* focused) override {
        auto* owner=findParentComponentOfClass<juce::AudioProcessorEditor>();
        if(owner && owner->getPeer() && focused!=owner && !owner->isParentOf(focused)) cancelTransientUi();
    }
    void parentHierarchyChanged() override {cancelTransientUi();}
    void timerCallback() override {
        const bool showing=isShowing();const bool focused=getPeer() && getPeer()->isFocused();
        if((was_showing_ && !showing) || (was_focused_ && !focused)) cancelTransientUi();
        was_showing_=showing;was_focused_=focused;refreshProgram();
        owned_dialogs_.erase(std::remove_if(owned_dialogs_.begin(),owned_dialogs_.end(),[](auto dialog){return dialog==nullptr;}),owned_dialogs_.end());
    }
    NativeHallProcessor& processor_;
    juce::Image background_,bezel_;
    InstrumentLook look_;
    juce::TooltipWindow tooltips_{this,650};
    Display display_;
    std::array<MotorFader,9> sliders_;
    std::array<MotorFader,6> parked_;
    std::array<MotorFader,6> xl_sliders_;
    std::unique_ptr<juce::ParameterAttachment> dynamic_attachment_;
    std::array<std::unique_ptr<juce::ParameterAttachment>,6> xl_attachments_;
    bool binding_xl_=false,bound_ready_=false;
    std::array<bool,6> xl_dragging_{};
    std::array<unsigned,6> xl_scales_{};
    unsigned focused_xl_slot_=0;
    int bound_program_=-1,bound_page_=-1;
    bool displayed_xl_=false;
    int page_=0;
    MotorFader dirt_;
    std::array<QuickPresetKey,NativeHallProcessor::quick_preset_count> quick_keys_;
    bool binding_dirt_=false,dragging_dirt_=false;
    RomSetup rom_setup_;
    SettingsButton settings_button_;
    SettingsPanel settings_panel_;
    PresetBrowser preset_browser_;
    std::array<std::unique_ptr<SliderAttachment>,9> slider_attachments_;
    std::unique_ptr<juce::ParameterAttachment> algorithm_attachment_;
    std::array<std::unique_ptr<juce::ParameterAttachment>,2> output_attachments_;
    std::array<std::unique_ptr<ButtonAttachment>,2> button_attachments_;
    std::unique_ptr<juce::ParameterAttachment> dirt_attachment_;
    juce::Component::SafePointer<PresetDialog> preset_dialog_;
    std::vector<juce::Component::SafePointer<PresetDialog>> owned_dialogs_;
    bool preset_dialog_open_=false;
    bool was_showing_=false,was_focused_=false;
    unsigned focused_=5;
    int displayed_program_=-1;
};

CineolEditor::CineolEditor(NativeHallProcessor& processor):AudioProcessorEditor(processor),panel_(std::make_unique<Panel>(processor)) {
    addAndMakeVisible(*panel_);setResizable(true,true);setResizeLimits(minimum_editor_width,minimum_editor_height,panel_width,panel_height);
    getConstrainer()->setFixedAspectRatio(double(panel_width)/panel_height);setSize(minimum_editor_width,minimum_editor_height);
}
CineolEditor::~CineolEditor()=default;
void CineolEditor::resized() {
    panel_->setTransform(juce::AffineTransform::scale(float(getWidth())/panel_width,float(getHeight())/panel_height));
}
