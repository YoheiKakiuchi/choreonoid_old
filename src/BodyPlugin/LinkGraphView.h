/**
   @author Shin'ichiro Nakaoka
*/

#ifndef CNOID_BODY_PLUGIN_LINK_GRAPH_VIEW_H
#define CNOID_BODY_PLUGIN_LINK_GRAPH_VIEW_H

#include "BodyItem.h"
#include <cnoid/Buttons>
#include <cnoid/MultiSE3SeqItem>
#include <cnoid/View>
#include <cnoid/GraphWidget>
#include <cnoid/ItemList>
#include <cnoid/ConnectionSet>
#include <QBoxLayout>
#include <set>

namespace cnoid {

class Archive;

/**
   @todo Define and implement the API for installing an index selection interface
   and move this class into GuiBase module
*/
class LinkGraphView : public View
{
public:
    static void initializeClass(ExtensionManager* ext);
        
    LinkGraphView();
    ~LinkGraphView();
            
    virtual bool storeState(Archive& archive);
    virtual bool restoreState(const Archive& archive);
            
protected:
    virtual QWidget* indicatorOnInfoBar();
            
private:
    GraphWidget graph;
    ToggleToolButton xyzToggles[3];
    ToggleToolButton rpyToggles[3];
    ConnectionSet toggleConnections;
    Connection rootItemConnection;

    struct ItemInfo
    {
        ~ItemInfo(){
            connections.disconnect();
        }
        MultiSE3SeqItemPtr item;
        std::shared_ptr<MultiSE3Seq> seq;
        BodyItemPtr bodyItem;
        ConnectionSet connections;
        std::vector<GraphDataHandlerPtr> handlers;
    };

    std::list<ItemInfo> itemInfos;

    std::set<BodyItemPtr> bodyItems;
    ConnectionSet bodyItemConnections;

    void setupElementToggleSet(QBoxLayout* box, ToggleToolButton toggles[], const char* labels[], bool isActive);
    void onSelectedItemsChanged(ItemList<MultiSE3SeqItem> items);
    void onDataItemDisconnectedFromRoot(std::list<ItemInfo>::iterator itemInfoIter);
    void updateBodyItems();
    void onBodyItemDisconnectedFromRoot(BodyItemPtr bodyItem);
    void setupGraphWidget();
    void addPositionTrajectory(std::list<ItemInfo>::iterator itemInfoIter, Link* link, std::shared_ptr<MultiSE3Seq> seq);
    void onDataItemUpdated(std::list<ItemInfo>::iterator itemInfoIter);

    void onDataRequest(
        std::list<ItemInfo>::iterator itemInfoIter,
        int linkIndex, int type, int axis, int frame, int size, double* out_values);
    void onDataModified(
        std::list<ItemInfo>::iterator itemInfoIter,
        int linkIndex, int type, int axis, int frame, int size, double* values);
};

}

#endif
