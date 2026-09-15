// PoseSkeleton.h — Single source of truth for the COCO-17 human skeleton edges.
//
// Real-time OSD (StreamViewerLiveData) and the PTask/PTaskBase image-upload and
// alarm-render path (PTaskBaseUpload) both draw the same COCO-17 topology. Keeping
// two private copies in those translation units lets them drift apart (a joint is
// added/renumbered in one renderer but not the other), which shows up as missing or
// misplaced limbs on only one output. Both renderers therefore share this table.
//
// The index is the COCO-17 keypoint slot (0=Nose,1/2=Eyes ... 15/16=ankles). Only
// boxes that cleared visibility checks at draw time are connected, so occluded
// limbs are skipped here purely by the caller deciding which slots are valid.

#pragma once

namespace cosmo {

// COCO-17 human pose skeleton connectivity, as pairs of keypoint slot indices.
inline constexpr int kCocoPoseEdges[][2] = {
    {5, 6},   {5, 7},   {7, 9},   {6, 8}, {8, 10}, {5, 11}, {6, 12}, {11, 12}, {11, 13},
    {13, 15}, {12, 14}, {14, 16}, {0, 1}, {0, 2},  {1, 3},  {2, 4},  {0, 5},   {0, 6},
};

// Number of keypoints a COCO-17 human pose model must supply. Renderers use this
// (instead of a bare magic 17) to recognise a one-stage pose target.
inline constexpr int kCocoPoseJointCount = 17;

}  // namespace cosmo
