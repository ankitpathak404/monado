// DemoHands.cs
// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2024 Nick Klingensmith
// Copyright (c) 2023-2024 Qualcomm Technologies, Inc.

using StereoKit;
using StereoKit.Framework;
using System;

namespace SKProjectName;

class DemoHands
{
    HandMenuRadial? handMenu;

    public void Initialize()
    {
        optionsPose = new Pose(-0.4f, 0, -0.4f, Quat.LookDir(1, 0, 1));
        dataPose    = new Pose( 0.4f, 0, -0.4f, Quat.LookDir(-1, 0, 1));

        handMenu = SK.AddStepper(new HandMenuRadial(
            new HandRadialLayer("Root",
                new HandMenuItem("File",   null, null, "File"),
                new HandMenuItem("Search", null, null, "Edit"),
                new HandMenuItem("Cancel", null, null)),
            new HandRadialLayer("File",
                new HandMenuItem("New",   null, () => Log.Info("New")),
                new HandMenuItem("Open",  null, () => Log.Info("Open")),
                new HandMenuItem("Close", null, () => Log.Info("Close")),
                new HandMenuItem("Back",  null, null, HandMenuAction.Back)),
            new HandRadialLayer("Edit",
                new HandMenuItem("Copy",  null, () => Log.Info("Copy")),
                new HandMenuItem("Paste", null, () => Log.Info("Paste")),
                new HandMenuItem("Back",  null, null, HandMenuAction.Back))));
    }

    public void Shutdown()
    {
        if (handMenu != null)
            SK.RemoveStepper(handMenu);
    }

    public void Step()
    {
        ShowHandData();
        ShowHandOptions();
        // Passthrough rendering is handled entirely in Program.cs.
        // Nothing extra needed here — hands & UI render on top automatically.
    }

    Pose   optionsPose = new Pose(-0.2f, 0, 0);
    Pose   dataPose    = new Pose( 0.2f, 0, 0);
    Handed activeHand  = Handed.Right;

    bool showHands     = true;
    bool showJoints    = false;
    bool showAxes      = true;
    bool showPointers  = true;
    bool showHandMenus = true;
    bool showHandSize  = true;
    bool showPinchPt   = true;

    void ShowHandOptions()
    {
        Vec2 size = V.XY(8, 0) * U.cm;

        UI.WindowBegin("Options", ref optionsPose, new Vec2(0, 0) * U.cm);

        UI.Label($"Hand source: {Input.HandSource(Handed.Right)}");

        UI.PanelBegin(UIPad.Inside);
        UI.Label("Show");
        if (UI.Toggle("Hands",     ref showHands,     size))
            Input.HandVisible(Handed.Max, showHands);
        UI.SameLine();
        UI.Toggle("Joints",        ref showJoints,    size);
        UI.SameLine();
        UI.Toggle("Axes",          ref showAxes,      size);

        UI.Toggle("Hand Size",     ref showHandSize,  size);
        UI.SameLine();
        UI.Toggle("Pointers",      ref showPointers,  size);
        UI.SameLine();
        UI.Toggle("Menu",          ref showHandMenus, size);

        UI.Toggle("Pinch Pt",      ref showPinchPt,   size);
        UI.PanelEnd();

        UI.HSeparator();

        UI.PanelBegin(UIPad.Inside);
        UI.Label("Color");
        if (UI.Button("Rainbow", size))
            ColorizeFingers(16, true,
                new Gradient(
                    new GradientKey(Color.HSV(0.0f, 1, 1), 0.1f),
                    new GradientKey(Color.HSV(0.2f, 1, 1), 0.3f),
                    new GradientKey(Color.HSV(0.4f, 1, 1), 0.5f),
                    new GradientKey(Color.HSV(0.6f, 1, 1), 0.7f),
                    new GradientKey(Color.HSV(0.8f, 1, 1), 0.9f)),
                new Gradient(
                    new GradientKey(new Color(1, 1, 1, 0),   0),
                    new GradientKey(new Color(1, 1, 1, 0), 0.4f),
                    new GradientKey(new Color(1, 1, 1, 1), 0.9f)));
        UI.SameLine();
        if (UI.Button("Normal", size))
            ColorizeFingers(16, true,
                new Gradient(new GradientKey(new Color(1, 1, 1, 1), 1)),
                new Gradient(
                    new GradientKey(new Color(.4f, .4f, .4f, 0),    0),
                    new GradientKey(new Color(.6f, .6f, .6f, 0), 0.4f),
                    new GradientKey(new Color(.8f, .8f, .8f, 1), 0.55f),
                    new GradientKey(new Color(1,   1,   1,   1),    1)));

        if (UI.Button("Black", size))
            ColorizeFingers(16, true,
                new Gradient(new GradientKey(new Color(0, 0, 0, 1), 1)),
                new Gradient(
                    new GradientKey(new Color(1, 1, 1, 0),   0),
                    new GradientKey(new Color(1, 1, 1, 0), 0.4f),
                    new GradientKey(new Color(1, 1, 1, 1), 0.6f),
                    new GradientKey(new Color(1, 1, 1, 1), 0.9f)));
        UI.SameLine();
        if (UI.Button("Full Black", size))
            ColorizeFingers(16, true,
                new Gradient(new GradientKey(new Color(0, 0, 0, 1), 1)),
                new Gradient(
                    new GradientKey(new Color(1, 1, 1, 0),    0),
                    new GradientKey(new Color(1, 1, 1, 1), 0.05f),
                    new GradientKey(new Color(1, 1, 1, 1),  1.0f)));
        UI.SameLine();
        if (UI.Button("Cutout Black", size))
            ColorizeFingers(16, false,
                new Gradient(new GradientKey(new Color(0, 0, 0, 0), 1)),
                new Gradient(new GradientKey(new Color(0, 0, 0, 0), 1)));

        UI.PanelEnd();
        UI.WindowEnd();

        if (showJoints)    DrawJoints(Mesh.Sphere, Default.Material);
        if (showAxes)      DrawAxes();
        if (showPointers)  DrawPointers();
        if (showHandSize)  DrawHandSize();
        if (showHandMenus)
        {
            DrawHandMenu(Handed.Right);
            DrawHandMenu(Handed.Left);
        }
        if (showPinchPt)
        {
            Hand l = Input.Hand(Handed.Left);
            Hand r = Input.Hand(Handed.Right);
            if (l.IsTracked) Mesh.Sphere.Draw(Default.Material, Matrix.TS(l.pinchPt, 0.005f));
            if (r.IsTracked) Mesh.Sphere.Draw(Default.Material, Matrix.TS(r.pinchPt, 0.005f));
        }
    }

    void ShowHandData()
    {
        UI.WindowBegin("Raw Data", ref dataPose, new Vec2(0.4f, 0));
        if (UI.Radio("Left",  activeHand == Handed.Left))  activeHand = Handed.Left;
        UI.SameLine();
        if (UI.Radio("Right", activeHand == Handed.Right)) activeHand = Handed.Right;

        Hand hand = Input.Hand(activeHand);
        LineItem("Tracked",  hand.IsTracked.ToString());
        LineItem("Pinch %",  $"{hand.pinchActivation:0.00}");
        LineItem("Pinched",  hand.pinch.IsActive().ToString());
        LineItem("Grip %",   $"{hand.gripActivation:0.00}");
        LineItem("Gripped",  hand.grip.IsActive().ToString());
        LineItem("Palm",     hand.palm.ToString());
        LineItem("Wrist",    hand.wrist.ToString());
        LineItem("Size",     $"{hand.size * 100:0.00}cm");

        UI.WindowEnd();
    }

    static void LineItem(string label, string content)
    {
        UI.Label(label, new Vec2(UI.LineHeight * 3, 0));
        UI.SameLine();
        UI.Label(content);
    }

    void ColorizeFingers(int size, bool transparent, Gradient horizontal, Gradient vertical)
    {
        Tex tex = new Tex(TexType.Image, TexFormat.Rgba32Linear);
        tex.AddressMode = TexAddress.Clamp;

        Color32[] pixels = new Color32[size * size];
        for (int y = 0; y < size; y++)
        {
            Color v = vertical.Get(1 - y / (size - 1.0f));
            for (int x = 0; x < size; x++)
            {
                Color h = horizontal.Get(x / (size - 1.0f));
                pixels[x + y * size] = v * h;
            }
        }
        tex.SetColors(size, size, pixels);

        Default.MaterialHand[MatParamName.DiffuseTex] = tex;
        Default.MaterialHand.Transparency = transparent
            ? Transparency.Blend
            : Transparency.None;
    }

    static bool HandFacingHead(Handed handed)
    {
        Hand hand = Input.Hand(handed);
        if (!hand.IsTracked) return false;

        Vec3 palmDirection   = hand.palm.Forward.Normalized;
        Vec3 directionToHead = (Input.Head.position - hand.palm.position).Normalized;

        return Vec3.Dot(palmDirection, directionToHead) > 0.5f;
    }

    public static void DrawHandMenu(Handed handed)
    {
        if (!HandFacingHead(handed)) return;

        Vec2  size   = new Vec2(4, 16);
        float offset = handed == Handed.Left ? -2 - size.x : 2 + size.x;

        Hand hand   = Input.Hand(handed);
        Vec3 at     = hand[FingerId.Little, JointId.KnuckleMajor].position;
        Vec3 down   = hand[FingerId.Little, JointId.Root         ].position;
        Vec3 across = hand[FingerId.Index,  JointId.KnuckleMajor ].position;

        Pose menuPose = new Pose(
            at,
            Quat.LookAt(at, across, at - down) * Quat.FromAngles(0, handed == Handed.Left ? 90 : -90, 0));
        menuPose.position += menuPose.Right * offset * U.cm;
        menuPose.position += menuPose.Up    * (size.y / 2) * U.cm;

        UI.WindowBegin("HandMenu", ref menuPose, size * U.cm, UIWin.Empty);
        UI.Button("Test");
        UI.Button("That");
        UI.Button("Hand");
        UI.WindowEnd();
    }

    public static void DrawAxes()
    {
        for (int i = 0; i < (int)Handed.Max; i++)
        {
            Hand hand = Input.Hand((Handed)i);
            if (!hand.IsTracked) continue;
            for (int finger = 0; finger < 5; finger++)
            for (int joint  = 0; joint  < 5; joint++)
                Lines.AddAxis(hand[finger, joint].Pose);
            Lines.AddAxis(hand.palm);
            Lines.AddAxis(hand.wrist);
        }
    }

    public static void DrawJoints(Mesh jointMesh, Material jointMaterial)
    {
        for (int i = 0; i < (int)Handed.Max; i++)
        {
            Hand hand = Input.Hand((Handed)i);
            if (!hand.IsTracked) continue;
            for (int finger = 0; finger < 5; finger++)
            for (int joint  = 0; joint  < 5; joint++)
            {
                HandJoint j = hand[finger, joint];
                jointMesh.Draw(jointMaterial, Matrix.TRS(j.position, j.orientation, j.radius / 2));
            }
        }
    }

    public static void DrawPointers()
    {
        int hands = Input.PointerCount(InputSource.Hand);
        for (int i = 0; i < hands; i++)
        {
            Pointer pointer = Input.Pointer(i, InputSource.Hand);
            Lines.Add    (pointer.ray, 0.5f, Color.White, Units.mm2m);
            Lines.AddAxis(pointer.Pose);
        }
    }

    public static void DrawHandSize()
    {
        for (int h = 0; h < (int)Handed.Max; h++)
        {
            Hand hand = Input.Hand((Handed)h);
            if (!hand.IsTracked) continue;

            HandJoint at  = hand[FingerId.Middle, JointId.Tip];
            Vec3      pos = at.position + at.Pose.Forward * at.radius;
            Quat      rot = at.orientation * Quat.FromAngles(-90, 0, 0);
            if (!HandFacingHead((Handed)h)) rot *= Quat.FromAngles(0, 180, 0);

            Text.Add(
                (hand.size * 100).ToString(".0") + "cm",
                Matrix.TRS(pos, rot, 0.3f),
                TextAlign.XCenter | TextAlign.YBottom);
        }
    }
}