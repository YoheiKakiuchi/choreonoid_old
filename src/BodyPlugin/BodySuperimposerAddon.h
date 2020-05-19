#ifndef CNOID_BODY_PLUGIN_BODY_SUPERIMPOSER_ADDON_H
#define CNOID_BODY_PLUGIN_BODY_SUPERIMPOSER_ADDON_H

#include <cnoid/ItemAddon>
#include "exportdecl.h"

namespace cnoid {

class Body;

class CNOID_EXPORT BodySuperimposerAddon : public ItemAddon
{
public:
    static void initializeClass(ExtensionManager* ext);
        
    BodySuperimposerAddon();
    BodySuperimposerAddon(const BodySuperimposerAddon& org) = delete;
    virtual ~BodySuperimposerAddon();

    virtual bool setOwnerItem(Item* item) override;

    int numSuperimposedBodies() const;
    Body* superimposedBody(int index);
    void setTransparency(float transparency);
    void updateSuperimposition();
    void clearSuperimposition();

protected:
    virtual bool store(Archive& archive) override;
    virtual bool restore(const Archive& archive) override;
            
private:
    class Impl;
    Impl* impl;
};

typedef ref_ptr<BodySuperimposerAddon> BodySuperimposerAddonPtr;

}

#endif
