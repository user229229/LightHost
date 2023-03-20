//
//  IconMenu.cpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//
//

#include "JuceHeader.h"
#include "IconMenu.hpp"
#include "PluginWindow.h"
#include <ctime>
#include <limits.h>
#if JUCE_WINDOWS
#include "Windows.h"
#endif

class IconMenu::PluginListWindow : public DocumentWindow
{
public:
	PluginListWindow(IconMenu& owner_, AudioPluginFormatManager& pluginFormatManager)
		: DocumentWindow("Available Plugins", Colours::white,
			DocumentWindow::minimiseButton | DocumentWindow::closeButton),
		owner(owner_)
	{
		const File deadMansPedalFile(getAppProperties().getUserSettings()
			->getFile().getSiblingFile("RecentlyCrashedPluginsList"));

		setContentOwned(new PluginListComponent(pluginFormatManager,
			owner.knownPluginList,
			deadMansPedalFile,
			getAppProperties().getUserSettings()), true);

		setUsingNativeTitleBar(true);
		setResizable(true, false);
		setResizeLimits(300, 400, 800, 1500);
		setTopLeftPosition(60, 60);

		restoreWindowStateFromString(getAppProperties().getUserSettings()->getValue("listWindowPos"));
		setVisible(true);
	}

	~PluginListWindow()
	{
		getAppProperties().getUserSettings()->setValue("listWindowPos", getWindowStateAsString());

		clearContentComponent();
	}

	void closeButtonPressed()
	{
        owner.removePluginsLackingInputOutput();
        #if JUCE_MAC
        Process::setDockIconVisible(false);
        #endif
		owner.pluginListWindow = nullptr;
	}

private:
	IconMenu& owner;
};

IconMenu::IconMenu() : INDEX_EDIT(1000000), INDEX_BYPASS(2000000), INDEX_DELETE(3000000), INDEX_MOVE_UP(4000000), INDEX_MOVE_DOWN(5000000)
{
    // Initiialization
    formatManager.addDefaultFormats();
    // Audio device
    std::unique_ptr<XmlElement> savedAudioState (getAppProperties().getUserSettings()->getXmlValue("audioDeviceState"));
    deviceManager.initialise(256, 256, savedAudioState.get(), true);
    player.setProcessor(&graph);
    deviceManager.addAudioCallback(&player);
    // Plugins - all
    std::unique_ptr<XmlElement> savedPluginList(getAppProperties().getUserSettings()->getXmlValue("pluginList"));
    if (savedPluginList != nullptr)
        knownPluginList.recreateFromXml(*savedPluginList);
    pluginSortMethod = KnownPluginList::sortByManufacturer;
    knownPluginList.addChangeListener(this);
    // Plugins - active
    std::unique_ptr<XmlElement> savedPluginListActive(getAppProperties().getUserSettings()->getXmlValue("pluginListActive"));
    if (savedPluginListActive != nullptr)
        activePluginList.recreateFromXml(*savedPluginListActive);
    loadActivePlugins();
    activePluginList.addChangeListener(this);
	setIcon();
	setIconTooltip(JUCEApplication::getInstance()->getApplicationName());
};

IconMenu::~IconMenu()
{
	savePluginStates();
}

void IconMenu::setIcon()
{
	// Set menu icon
	Image icon;
	#if JUCE_MAC
		if (exec("defaults read -g AppleInterfaceStyle").compare("Dark") == 1)
		    icon = ImageFileFormat::loadFrom(BinaryData::menu_icon_white_png, BinaryData::menu_icon_white_pngSize);
		else
			icon = ImageFileFormat::loadFrom(BinaryData::menu_icon_png, BinaryData::menu_icon_pngSize);
		setIconImage(icon, icon);
	#else
		String defaultColor;
	#if JUCE_WINDOWS
		defaultColor = "white";
	#elif JUCE_LINUX
		defaultColor = "black";
	#endif
		if (!getAppProperties().getUserSettings()->containsKey("icon"))
			getAppProperties().getUserSettings()->setValue("icon", defaultColor);
		String color = getAppProperties().getUserSettings()->getValue("icon");
		if (color.equalsIgnoreCase("white"))
			icon = ImageFileFormat::loadFrom(BinaryData::menu_icon_white_png, BinaryData::menu_icon_white_pngSize);
		else if (color.equalsIgnoreCase("black"))
			icon = ImageFileFormat::loadFrom(BinaryData::menu_icon_png, BinaryData::menu_icon_pngSize);
		setIconImage(icon, icon);
	#endif
}

void IconMenu::loadActivePlugins()
{
	const int INPUT = 1000000;
	const int OUTPUT = INPUT + 1;
	const int CHANNEL_ONE = 0;
	const int CHANNEL_TWO = 1;
	PluginWindow::closeAllCurrentlyOpenWindows();
    graph.clear();
	inputNode = graph.addNode(std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode), AudioProcessorGraph::NodeID(INPUT));
    outputNode = graph.addNode(std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode), AudioProcessorGraph::NodeID(OUTPUT));
    if (activePluginList.getNumTypes() == 0)
    {
		graph.addConnection ({ { AudioProcessorGraph::NodeID(INPUT), CHANNEL_ONE }, { AudioProcessorGraph::NodeID(OUTPUT), CHANNEL_ONE } });
		graph.addConnection ({ { AudioProcessorGraph::NodeID(INPUT), CHANNEL_TWO }, { AudioProcessorGraph::NodeID(OUTPUT), CHANNEL_TWO } });
    }
	int pluginTime = 0;
	int lastId = 0;
	bool hasInputConnected = false;
	// NOTE: Node ids cannot begin at 0.
    for (int i = 1; i <= activePluginList.getNumTypes(); i++)
    {
        PluginDescription plugin = getNextPluginOlderThanTime(pluginTime);
        String errorMessage;
        std::unique_ptr<AudioPluginInstance> instance = formatManager.createPluginInstance(plugin, graph.getSampleRate(), graph.getBlockSize(), errorMessage);
		String pluginUid = getKey("state", plugin);
        String savedPluginState = getAppProperties().getUserSettings()->getValue(pluginUid);
        MemoryBlock savedPluginBinary;
        savedPluginBinary.fromBase64Encoding(savedPluginState);
        instance->setStateInformation(savedPluginBinary.getData(), savedPluginBinary.getSize());
        graph.addNode(std::move(instance), AudioProcessorGraph::NodeID(i)); // TODO https://stackoverflow.com/a/17473958
		String key = getKey("bypass", plugin);
		bool bypass = getAppProperties().getUserSettings()->getBoolValue(key, false);
        // Input to plugin
        if ((!hasInputConnected) && (!bypass))
        {
            graph.addConnection({ { AudioProcessorGraph::NodeID(INPUT), CHANNEL_ONE }, { AudioProcessorGraph::NodeID(i), CHANNEL_ONE } });
            graph.addConnection({ { AudioProcessorGraph::NodeID(INPUT), CHANNEL_TWO }, { AudioProcessorGraph::NodeID(i), CHANNEL_TWO } });
			hasInputConnected = true;
        }
        // Connect previous plugin to current
        else if ((!bypass))
        {
            graph.addConnection({ { AudioProcessorGraph::NodeID(lastId), CHANNEL_ONE }, { AudioProcessorGraph::NodeID(i), CHANNEL_ONE } });
            graph.addConnection({ { AudioProcessorGraph::NodeID(lastId), CHANNEL_TWO }, { AudioProcessorGraph::NodeID(i), CHANNEL_TWO } });
        }
		if (!bypass)
		    lastId = i;
    }
	if (lastId > 0)
	{
		// Last active plugin to output
		graph.addConnection({ { AudioProcessorGraph::NodeID(lastId), CHANNEL_ONE }, { AudioProcessorGraph::NodeID(OUTPUT), CHANNEL_ONE } });
		graph.addConnection({ { AudioProcessorGraph::NodeID(lastId), CHANNEL_TWO }, { AudioProcessorGraph::NodeID(OUTPUT), CHANNEL_TWO } });
	}
}

PluginDescription IconMenu::getNextPluginOlderThanTime(int &time)
{
	int timeStatic = time;
	PluginDescription closest;
	int diff = INT_MAX;
	for (int i = 0; i < activePluginList.getNumTypes(); i++)
	{
		PluginDescription plugin = *activePluginList.getType(i);
		String key = getKey("order", plugin);
		String pluginTimeString = getAppProperties().getUserSettings()->getValue(key);
		int pluginTime = atoi(pluginTimeString.toStdString().c_str());
		if (pluginTime > timeStatic && abs(timeStatic - pluginTime) < diff)
		{
			diff = abs(timeStatic - pluginTime);
			closest = plugin;
			time = pluginTime;
		}
	}
	return closest;
}

void IconMenu::changeListenerCallback(ChangeBroadcaster* changed)
{
    if (changed == &knownPluginList)
    {
        std::unique_ptr<XmlElement> savedPluginList (knownPluginList.createXml());
        if (savedPluginList != nullptr)
        {
            getAppProperties().getUserSettings()->setValue ("pluginList", savedPluginList.get());
            getAppProperties().saveIfNeeded();
        }
    }
    else if (changed == &activePluginList)
    {
        std::unique_ptr<XmlElement> savedPluginList (activePluginList.createXml());
        if (savedPluginList != nullptr)
        {
            getAppProperties().getUserSettings()->setValue ("pluginListActive", savedPluginList.get());
            getAppProperties().saveIfNeeded();
        }
    }
}

#if JUCE_MAC
std::string IconMenu::exec(const char* cmd)
{
    std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
    if (!pipe) return "ERROR";
    char buffer[128];
    std::string result = "";
    while (!feof(pipe.get()))
    {
        if (fgets(buffer, 128, pipe.get()) != NULL)
            result += buffer;
    }
    return result;
}
#endif

void IconMenu::timerCallback()
{
    stopTimer();
    menu.clear();
    menu.addSectionHeader(JUCEApplication::getInstance()->getApplicationName());
    if (menuIconLeftClicked) {
        menu.addItem(1, "Preferences");
        menu.addItem(2, "Edit Plugins");
        menu.addSeparator();
		menu.addSectionHeader("Active Plugins");
        // Active plugins
		int time = 0;
        for (int i = 0; i < activePluginList.getNumTypes(); i++)
        {
            PopupMenu options;
            options.addItem(INDEX_EDIT + i, "Edit");
			std::vector<PluginDescription> timeSorted = getTimeSortedList();
			String key = getKey("bypass", timeSorted[i]);
			bool bypass = getAppProperties().getUserSettings()->getBoolValue(key);
			options.addItem(INDEX_BYPASS + i, "Bypass", true, bypass);
			options.addSeparator();
			options.addItem(INDEX_MOVE_UP + i, "Move Up", i > 0);
			options.addItem(INDEX_MOVE_DOWN + i, "Move Down", i < timeSorted.size() - 1);
			options.addSeparator();
            options.addItem(INDEX_DELETE + i, "Delete");
			PluginDescription plugin = getNextPluginOlderThanTime(time);
            menu.addSubMenu(plugin.name, options);
        }
        menu.addSeparator();
		menu.addSectionHeader("Avaliable Plugins");
        // All plugins
        knownPluginList.addToMenu(menu, pluginSortMethod);
    }
    else
    {
        menu.addItem(1, "Quit");
		menu.addSeparator();
		menu.addItem(2, "Delete Plugin States");
		#if !JUCE_MAC
			menu.addItem(3, "Invert Icon Color");
		#endif
    }
	#if JUCE_MAC || JUCE_LINUX
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(this), ModalCallbackFunction::forComponent(menuInvocationCallback, this));
	#else
	menu.showMenuAsync(PopupMenu::Options().withMousePosition(), ModalCallbackFunction::forComponent(menuInvocationCallback, this));
	#endif
}

void IconMenu::mouseDown(const MouseEvent& e)
{
	#if JUCE_MAC
		Process::setDockIconVisible(true);
	#endif
    Process::makeForegroundProcess();
    menuIconLeftClicked = e.mods.isLeftButtonDown();
    startTimer(50);
}

void IconMenu::menuInvocationCallback(int id, IconMenu* im)
{
    // Right click
    if ((!im->menuIconLeftClicked))
    {
		if (id == 1)
		{
			im->savePluginStates();
			return JUCEApplication::getInstance()->quit();
		}
		if (id == 2)
		{
			im->deletePluginStates();
			return im->loadActivePlugins();
		}
		if (id == 3)
		{
			String color = getAppProperties().getUserSettings()->getValue("icon");
			getAppProperties().getUserSettings()->setValue("icon", color.equalsIgnoreCase("black") ? "white" : "black");
			return im->setIcon();
		}
    }
	#if JUCE_MAC
    // Click elsewhere
    if (id == 0 && !PluginWindow::containsActiveWindows())
        Process::setDockIconVisible(false);
	#endif
    // Audio settings
    if (id == 1)
        im->showAudioSettings();
    // Reload
    if (id == 2)
        im->reloadPlugins();
    // Plugins
    if (id > 2)
    {
        // Delete plugin
        if (id >= im->INDEX_DELETE && id < im->INDEX_DELETE + 1000000)
        {
            im->deletePluginStates();

			int index = id - im->INDEX_DELETE;
			std::vector<PluginDescription> timeSorted = im->getTimeSortedList();
			String key = getKey("order", timeSorted[index]);
			PluginDescription typeToRemove;
			for (int i = 0; im->activePluginList.getNumTypes(); i++)
			{
				typeToRemove = *im->activePluginList.getType(i);
				if (key.equalsIgnoreCase(getKey("order", typeToRemove)))
				{
					break;
				}
			}

			// Remove plugin order
			getAppProperties().getUserSettings()->removeValue(key);
			// Remove bypass entry
			getAppProperties().getUserSettings()->removeValue(getKey("bypass", timeSorted[index]));
			getAppProperties().saveIfNeeded();
			
			// Remove plugin from list
            im->activePluginList.removeType(typeToRemove);

			// Save current states
			im->savePluginStates();
			im->loadActivePlugins();
        }
        // Add plugin
        else if (im->knownPluginList.getIndexChosenByMenu(id) > -1)
        {
			PluginDescription plugin = *im->knownPluginList.getType(im->knownPluginList.getIndexChosenByMenu(id));
			String key = getKey("order", plugin);
			int t = time(0);
			getAppProperties().getUserSettings()->setValue(key, t);
			getAppProperties().saveIfNeeded();
            im->activePluginList.addType(plugin);

			im->savePluginStates();
			im->loadActivePlugins();
        }
		// Bypass plugin
		else if (id >= im->INDEX_BYPASS && id < im->INDEX_BYPASS + 1000000)
		{
			int index = id - im->INDEX_BYPASS;
			std::vector<PluginDescription> timeSorted = im->getTimeSortedList();
			String key = getKey("bypass", timeSorted[index]);

			// Set bypass flag
			bool bypassed = getAppProperties().getUserSettings()->getBoolValue(key);
			getAppProperties().getUserSettings()->setValue(key, !bypassed);
			getAppProperties().saveIfNeeded();

			im->savePluginStates();
			im->loadActivePlugins();
		}
        // Show active plugin GUI
		else if (id >= im->INDEX_EDIT && id < im->INDEX_EDIT + 1000000)
        {
            if (const AudioProcessorGraph::Node::Ptr f = im->graph.getNodeForId(AudioProcessorGraph::NodeID(id - im->INDEX_EDIT + 1)))
                if (PluginWindow* const w = PluginWindow::getWindowFor(f, PluginWindow::Normal))
                    w->toFront(true);
        }
		// Move plugin up the list
		else if (id >= im->INDEX_MOVE_UP && id < im->INDEX_MOVE_UP + 1000000)
		{
			im->savePluginStates();
			std::vector<PluginDescription> timeSorted = im->getTimeSortedList();
			PluginDescription toMove = timeSorted[id - im->INDEX_MOVE_UP];
			for (int i = 0; i < timeSorted.size(); i++)
			{
				bool move = getKey("move", toMove).equalsIgnoreCase(getKey("move", timeSorted[i]));
				getAppProperties().getUserSettings()->setValue(getKey("order", timeSorted[i]), move ? i : i+1);
				if (move)
					getAppProperties().getUserSettings()->setValue(getKey("order", timeSorted[i-1]), i+1);
			}
			im->loadActivePlugins();
		}
		// Move plugin down the list
		else if (id >= im->INDEX_MOVE_DOWN && id < im->INDEX_MOVE_DOWN + 1000000)
		{
			im->savePluginStates();
			std::vector<PluginDescription> timeSorted = im->getTimeSortedList();
			PluginDescription toMove = timeSorted[id - im->INDEX_MOVE_DOWN];
			for (int i = 0; i < timeSorted.size(); i++)
			{
				bool move = getKey("move", toMove).equalsIgnoreCase(getKey("move", timeSorted[i]));
				getAppProperties().getUserSettings()->setValue(getKey("order", timeSorted[i]), move ? i+2 : i+1);
				if (move)
				{
					getAppProperties().getUserSettings()->setValue(getKey("order", timeSorted[i + 1]), i + 1);
					i++;
				}
			}
			im->loadActivePlugins();
		}
        // Update menu
        im->startTimer(50);
    }
}

std::vector<PluginDescription> IconMenu::getTimeSortedList()
{
	int time = 0;
	std::vector<PluginDescription> list;
	for (int i = 0; i < activePluginList.getNumTypes(); i++)
		list.push_back(getNextPluginOlderThanTime(time));
	return list;
		
}

String IconMenu::getKey(String type, PluginDescription plugin)
{
	String key = "plugin-" + type.toLowerCase() + "-" + plugin.name + plugin.version + plugin.pluginFormatName;
	return key;
}

void IconMenu::deletePluginStates()
{
	std::vector<PluginDescription> list = getTimeSortedList();
    for (int i = 0; i < activePluginList.getNumTypes(); i++)
    {
		String pluginUid = getKey("state", list[i]);
        getAppProperties().getUserSettings()->removeValue(pluginUid);
        getAppProperties().saveIfNeeded();
    }
}

void IconMenu::savePluginStates()
{
	std::vector<PluginDescription> list = getTimeSortedList();
    for (int i = 0; i < activePluginList.getNumTypes(); i++)
    {
		AudioProcessorGraph::Node* node = graph.getNodeForId(AudioProcessorGraph::NodeID(i + 1));
		if (node == nullptr)
			break;
        AudioProcessor& processor = *node->getProcessor();
		String pluginUid = getKey("state", list[i]);
        MemoryBlock savedStateBinary;
        processor.getStateInformation(savedStateBinary);
        getAppProperties().getUserSettings()->setValue(pluginUid, savedStateBinary.toBase64Encoding());
        getAppProperties().saveIfNeeded();
    }
}

void IconMenu::showAudioSettings()
{
    AudioDeviceSelectorComponent audioSettingsComp (deviceManager, 0, 256, 0, 256, false, false, true, false);
    audioSettingsComp.setSize(500, 600);
    
    DialogWindow::LaunchOptions o;
    o.content.setNonOwned(&audioSettingsComp);
    o.dialogTitle                   = "Audio Settings";
    o.componentToCentreAround       = this;
    o.dialogBackgroundColour        = Colour::fromRGB(236, 236, 236);
    o.escapeKeyTriggersCloseButton  = true;
    o.useNativeTitleBar             = true;
    o.resizable                     = false;

    o.runModal();
        
    std::unique_ptr<XmlElement> audioState(deviceManager.createStateXml());
        
    getAppProperties().getUserSettings()->setValue("audioDeviceState", audioState.get());
    getAppProperties().getUserSettings()->saveIfNeeded();
}

void IconMenu::reloadPlugins()
{
	if (pluginListWindow == nullptr)
		pluginListWindow = new PluginListWindow(*this, formatManager);
	pluginListWindow->toFront(true);
}

void IconMenu::removePluginsLackingInputOutput()
{
	// TODO needs sanity check
    for (const auto& plugin : knownPluginList.getTypes())
    {
        if (plugin.numInputChannels < 2 || plugin.numOutputChannels < 2)
		    knownPluginList.removeType(plugin);
    }
}