#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../dsp/FilterManager.h"
#include <vector>

namespace LiveAudioLab
{

class FilterListView : public juce::Component,
                       public juce::TableListBoxModel,
                       public juce::Timer
{
public:
    FilterListView(FilterManager& managerRef);
    ~FilterListView() override;

    int getNumRows() override;
    void paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected) override;
    void paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
    juce::Component* refreshComponentForCell(int rowNumber, int columnId, bool isRowSelected,
                                            juce::Component* existingComponentToUpdate) override;

    void resized() override;
    void timerCallback() override;

private:
    FilterManager& filterManager;
    juce::TableListBox table;
    std::vector<FilterSlotInfo> activeFilters;

    juce::TextButton clearLiveButton { "Clear Live" };
    juce::TextButton lockAllButton { "Lock All to Fixed" };
    juce::TextButton clearAllButton { "Clear All" };

    enum Columns
    {
        ColIndex = 1,
        ColType,
        ColFreq,
        ColCut,
        ColQ,
        ColTimer,
        ColHits,
        ColActions
    };
};

} // namespace LiveAudioLab
