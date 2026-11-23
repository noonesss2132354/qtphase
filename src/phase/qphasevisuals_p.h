// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QPHASEVISUALS_P_H
#define QPHASEVISUALS_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include "qphasescenearena_p.h"
#include <vector>

QT_BEGIN_NAMESPACE

class QPhaseVisualsPrivate
{
public:
    QPhaseSceneArena *arena = nullptr;
    uint32_t transformIndex = 0;
    uint32_t clipIndex = 0;
    int activeLayerDepth = 0;
    std::vector<PhaseGpuPrimitive> recordedPrimitives;
};

QT_END_NAMESPACE

#endif // QPHASEVISUALS_P_H
