#include "FilterListView.h"

namespace LiveAudioLab
{

class ActionButtonsCell : public juce::Component
{
public:
    ActionButtonsCell(FilterManager& manager, int slotId, bool isLocked, bool isBypassed)
        : filterManager(manager), id(slotId), locked(isLocked), bypassed(isBypassed)
    {
        update(slotId, isLocked, isBypassed);

        lockButton.onClick = [this]() {
            filterManager.setFilterLocked(id, !locked);
        };

        bypassButton.onClick = [this]() {
            filterManager.setFilterBypassed(id, !bypassed);
        };

        deleteButton.onClick = [this]() {
            filterManager.deleteFilter(id);
        };

        addAndMakeVisible(lockButton);
        addAndMakeVisible(bypassButton);
        addAndMakeVisible(deleteButton);
    }

    void update(int newId, bool isLocked, bool isBypassed)
    {
        id = newId;
        locked = isLocked;
        bypassed = isBypassed;
        lockButton.setButtonText(locked ? "Unlock" : "Lock");
        bypassButton.setButtonText(bypassed ? "Unbypass" : "Bypass");
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(2);
        int btnW = (area.getWidth() - 8) / 3;
        lockButton.setBounds(area.removeFromLeft(btnW));
        area.removeFromLeft(4);
        bypassButton.setBounds(area.removeFromLeft(btnW));
        area.removeFromLeft(4);
        deleteButton.setBounds(area);
    }

private:
    FilterManager& filterManager;
    int id;
    bool locked = false;
    bool bypassed = false;
    juce::TextButton lockButton;
    juce::TextButton bypassButton;
    juce::TextButton deleteButton;
};

FilterListView::FilterListView(FilterManager& managerRef)
    : filterManager(managerRef)
{
    table.setModel(this);
    table.getHeader().addColumn("#", ColIndex, 36, 30, 45);
    table.getHeader().addColumn("Type", ColType, 60, 50, 70);
    table.getHeader().addColumn("Frequency", ColFreq, 85, 70, 110);
    table.getHeader().addColumn("Cut (dB)", ColCut, 80, 65, 100);
    table.getHeader().addColumn("Q", ColQ, 55, 45, 70);
    table.getHeader().addColumn("Lift / Status", ColTimer, 90, 70, 110);
    table.getHeader().addColumn("Hits", ColHits, 45, 35, 60);
    table.getHeader().addColumn("Actions", ColActions, 150, 120, 200);

    table.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff161920));
    table.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, juce::Colour(0xff1e232c));
    table.getHeader().setColour(juce::TableHeaderComponent::textColourId, juce::Colour(0xffa0abbc));

    addAndMakeVisible(table);

    clearLiveButton.onClick = [this]() { filterManager.clearAllLive(); };
    lockAllButton.onClick = [this]() { filterManager.lockAllLiveToFixed(); };
    clearAllButton.onClick = [this]() { filterManager.clearAll(); };

    addAndMakeVisible(clearLiveButton);
    addAndMakeVisible(lockAllButton);
    addAndMakeVisible(clearAllButton);

    startTimerHz(10); // 10 Hz table refresh
}

FilterListView::~FilterListView()
{
    stopTimer();
}

void FilterListView::timerCallback()
{
    activeFilters = filterManager.getFilterSnapshots();
    table.updateContent();
    table.repaint();
}

int FilterListView::getNumRows()
{
    return static_cast<int>(activeFilters.size());
}

void FilterListView::paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected)
{
    if (rowIsSelected)
        g.fillAll(juce::Colour(0xff00e5ff).withAlpha(0.15f));
    else if (rowNumber % 2 == 0)
        g.fillAll(juce::Colour(0xff161920));
    else
        g.fillAll(juce::Colour(0xff1a1e26));
}

void FilterListView::paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int>(activeFilters.size()))
        return;

    const auto& slot = activeFilters[static_cast<size_t>(rowNumber)];
    g.setFont(juce::FontOptions(11.0f));

    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(4, 2);

    switch (columnId)
    {
        case ColIndex:
        {
            g.setColour(juce::Colour(0xff7a889b));
            g.drawText(juce::String(rowNumber + 1), bounds, juce::Justification::centredLeft);
            break;
        }
        case ColType:
        {
            juce::Colour typeCol = slot.isFixed ? juce::Colour(0xffff9100) : juce::Colour(0xff00e5ff);
            g.setColour(typeCol.withAlpha(0.2f));
            g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
            g.setColour(typeCol);
            g.drawRoundedRectangle(bounds.toFloat(), 3.0f, 1.0f);
            g.drawText(slot.isFixed ? "FIXED" : "LIVE", bounds, juce::Justification::centred);
            break;
        }
        case ColFreq:
        {
            juce::String freqStr;
            if (slot.frequency >= 1000.0f)
                freqStr = juce::String(slot.frequency / 1000.0f, 2) + " kHz";
            else
                freqStr = juce::String(static_cast<int>(slot.frequency)) + " Hz";

            g.setColour(juce::Colours::white);
            g.drawText(freqStr, bounds, juce::Justification::centredLeft);
            break;
        }
        case ColCut:
        {
            juce::String cutStr = juce::String(slot.currentGainDb, 1) + " dB";
            g.setColour(juce::Colour(0xffff5252));
            g.drawText(cutStr, bounds, juce::Justification::centredLeft);
            break;
        }
        case ColQ:
        {
            g.setColour(juce::Colour(0xffc5cfe0));
            g.drawText(juce::String(slot.q, 1), bounds, juce::Justification::centredLeft);
            break;
        }
        case ColTimer:
        {
            if (slot.isFixed || slot.isLocked)
            {
                g.setColour(juce::Colour(0xffff9100));
                g.drawText(slot.isLocked ? "LOCKED" : "FIXED", bounds, juce::Justification::centredLeft);
            }
            else if (slot.isReleasing)
            {
                g.setColour(juce::Colour(0xff7a889b));
                g.drawText("releasing", bounds, juce::Justification::centredLeft);
            }
            else if (slot.liftDurationSec <= 0.0f)
            {
                g.setColour(juce::Colour(0xff00e5ff));
                g.drawText("HOLD", bounds, juce::Justification::centredLeft);
            }
            else
            {
                g.setColour(juce::Colour(0xff00e5ff));
                int sec = static_cast<int>(slot.liftRemainingSec);
                g.drawText(juce::String(sec) + "s lift", bounds, juce::Justification::centredLeft);
            }
            break;
        }
        case ColHits:
        {
            g.setColour(juce::Colour(0xffa0abbc));
            g.drawText(juce::String(slot.triggerCount) + "x", bounds, juce::Justification::centred);
            break;
        }
        default:
            break;
    }
}

juce::Component* FilterListView::refreshComponentForCell(int rowNumber, int columnId, bool,
                                                        juce::Component* existingComponentToUpdate)
{
    if (columnId == ColActions)
    {
        if (rowNumber >= 0 && rowNumber < static_cast<int>(activeFilters.size()))
        {
            const auto& slot = activeFilters[static_cast<size_t>(rowNumber)];
            auto* cell = dynamic_cast<ActionButtonsCell*>(existingComponentToUpdate);
            if (cell == nullptr)
            {
                cell = new ActionButtonsCell(filterManager, slot.id, slot.isLocked, slot.isBypassed);
            }
            else
            {
                cell->update(slot.id, slot.isLocked, slot.isBypassed);
            }
            return cell;
        }
    }
    delete existingComponentToUpdate;
    return nullptr;
}

void FilterListView::resized()
{
    auto area = getLocalBounds();

    // Top action bar for batch operations
    auto buttonBar = area.removeFromBottom(34).reduced(4, 2);
    int btnW = 120;
    clearLiveButton.setBounds(buttonBar.removeFromLeft(btnW));
    buttonBar.removeFromLeft(8);
    lockAllButton.setBounds(buttonBar.removeFromLeft(btnW + 20));
    buttonBar.removeFromLeft(8);
    clearAllButton.setBounds(buttonBar.removeFromLeft(btnW));

    table.setBounds(area);
}

} // namespace LiveAudioLab
