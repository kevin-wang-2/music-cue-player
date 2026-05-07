#include "AppModel.h"

AppModel::AppModel(QObject* parent)
    : QObject(parent)
    , scheduler(engine)
    , cues(engine, scheduler)
{}

AppModel::~AppModel() {
    cues.panic();
}

void AppModel::pushUndo() {
    if (sf.cueLists.empty()) return;
    undoStack.push_back(sf.cueLists[0].cues);
    if ((int)undoStack.size() > kMaxUndo)
        undoStack.erase(undoStack.begin());
    redoStack.clear();
}

void AppModel::tick() {
    const bool wasActive = engine.activeVoiceCount() > 0;
    cues.update();
    const bool isActive  = engine.activeVoiceCount() > 0;
    if (wasActive != isActive)
        emit playbackStateChanged();
}
