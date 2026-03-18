// Program.cs
using StereoKit;
using System;
using System.Net.Sockets;
using System.IO;
using System.Threading.Tasks;

namespace SKProjectName
{
    class Program
    {
        public static Tex? passthroughTex = null;
        public static Material passthroughMat = new Material(Shader.Unlit);
        static int frameCount = 0;

        public static int passthroughTexWidth = 0;
        public static int passthroughTexHeight = 0;

        static Pose passthroughInfoPose = new Pose();

        static void Main(string[] args)
        {
            SKSettings settings = new SKSettings
            {
                appName         = "SKProjectName",
                blendPreference = DisplayBlend.AnyTransparent,
            };
            if (!SK.Initialize(settings))
                return;

            passthroughMat.Transparency = Transparency.Blend;

            // --- Background Memory Mapped Frame receiver ---
            Task.Run(async () =>
            {
                System.IO.MemoryMappedFiles.MemoryMappedFile? mmf = null;
                System.IO.MemoryMappedFiles.MemoryMappedViewAccessor? accessor = null;
                
                int w = 640; 
                int h = 480;
                long frameSize = 0;

                while (mmf == null)
                {
                    try
                    {
                        // On Linux, named maps (OpenExisting) aren't supported by .NET.
                        // We must map the POSIX memory file directly from the tmpfs.
                        var fs = new FileStream("/dev/shm/monado_mp_frame", FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                        mmf = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateFromFile(
                            fs, null, 0, System.IO.MemoryMappedFiles.MemoryMappedFileAccess.Read, 
                            HandleInheritability.None, false);
                        
                        accessor = mmf.CreateViewAccessor(0, 0, System.IO.MemoryMappedFiles.MemoryMappedFileAccess.Read);
                        frameSize = accessor.Capacity;
                        
                        // Infer the resolution from byte size (w * h * 4 for RGBA)
                        if (frameSize == 1920 * 1080 * 4) { w = 1920; h = 1080; }
                        else if (frameSize == 1280 * 720 * 4) { w = 1280; h = 720; }
                        else if (frameSize == 640 * 480 * 4) { w = 640; h = 480; }
                        else if (frameSize == 640 * 360 * 4) { w = 640; h = 360; }
                        else { 
                            w = (int)Math.Sqrt(frameSize / 4.0 * (16.0 / 9.0));
                            h = (int)(w * 9.0 / 16.0);
                        }
                        
                        Log.Info($"[Passthrough] Connected to 'monado_mp_frame'. Capacity={frameSize}, Guessing {w}x{h} (RGBA)");
                    }
                    catch (Exception ex)
                    {
                        Log.Info($"[Passthrough] Waiting for shared memory... {ex.Message}");
                        mmf = null;
                        await Task.Delay(1000);
                    }
                }

                while (true)
                {
                    try
                    {
                        // True Zero-Copy block transmission from Shared Memory directly to the Graphics Card
                        // Bypasses the C# Garbage Collector entirely
                        IntPtr rawPtr = accessor.SafeMemoryMappedViewHandle.DangerousGetHandle();

                        SK.ExecuteOnMain(() =>
                        {
                            // Recreate texture if size doesn't match
                            if (passthroughTex == null || passthroughTexWidth != w || passthroughTexHeight != h)
                            {
                                passthroughTexWidth  = w;  
                                passthroughTexHeight = h;
                                passthroughTex = new Tex(TexType.ImageNomips, TexFormat.Rgba32);
                                passthroughMat[MatParamName.DiffuseTex] = passthroughTex;
                                Log.Info($"[Passthrough] Created {w}x{h} texture.");
                            }
                            passthroughTex.SetColors(passthroughTexWidth, passthroughTexHeight, rawPtr);
                            frameCount++;
                        });

                        await Task.Delay(16); // Target ~60fps reading
                    }
                    catch (Exception e)
                    {
                        Log.Warn("[Passthrough] Map read error: " + e.Message);
                        await Task.Delay(1000);
                    }
                }
            });

            // --- Scene ---
            Pose   cubePose       = new Pose(0, 0, -0.5f);
            Model  cube           = Model.FromMesh(Mesh.GenerateRoundedCube(Vec3.One * 0.1f, 0.02f), Material.UI);
            

            DemoHands hands = new DemoHands();
            hands.Initialize();

            SK.Run(() =>
            {
                Pose  head = Input.Head;

                // 1) Draw passthrough quad FIRST so everything renders on top
                if (passthroughTex != null)
                {
                    float aspect = passthroughTexHeight > 0
                        ? (float)passthroughTexWidth / passthroughTexHeight
                        : 16f / 9f;

                    float dist   = 2f;
                    float width  = dist * 2f * MathF.Tan(55f * MathF.PI / 180f); // 110° horizontal FOV
                    float height = width / aspect;

                    Vec3 quadPos = head.position + head.Forward * dist;

                    // LookAt so the quad always faces the user correctly in both eyes
                    Quat quadRot = Quat.LookAt(quadPos, head.position);

                    Mesh.Quad.Draw(passthroughMat, Matrix.TRS(quadPos, quadRot, new Vec3(width, height, 1f)));

                    // HUD info — also faces user
                    Vec3 hudPos = head.position + head.Forward * 0.6f + head.Up * 0.35f;
                    Quat hudRot = Quat.LookAt(hudPos, head.position);
                    passthroughInfoPose.position    = hudPos;
                    passthroughInfoPose.orientation = hudRot;
                    UI.WindowBegin("Passthrough Info", ref passthroughInfoPose, new Vec2(22, 0) * U.cm, UIWin.Empty);
                    UI.Text($"Frames: {frameCount}  |  {passthroughTexWidth}x{passthroughTexHeight}");
                    UI.WindowEnd();
                }

                // 2) Floor (opaque mode only)
                

                // 3) Interactive cube
                UI.Handle("Cube", ref cubePose, cube.Bounds);
                cube.Draw(cubePose.ToMatrix());

                // 4) Hand tracking on top of everything
                hands.Step();

            }, () =>
            {
                hands.Shutdown();
            });
        }

        static async Task<byte[]?> ReadExactAsync(BinaryReader reader, int count)
        {
            byte[] buffer = new byte[count];
            int offset = 0;
            try
            {
                var stream = reader.BaseStream;
                while (offset < count)
                {
                    int read = await stream.ReadAsync(buffer, offset, count - offset);
                    if (read == 0) return null;
                    offset += read;
                }
                return buffer;
            }
            catch (Exception e)
            {
                Log.Warn("ReadExactAsync failed: " + e.Message);
                return null;
            }
        }

        static bool TryGetJpegSize(byte[] data, out int width, out int height)
        {
            width = height = 0;
            if (data == null || data.Length < 10) return false;
            int i = 2;
            while (i + 9 < data.Length)
            {
                if (data[i] != 0xFF) { i++; continue; }
                int marker = data[i + 1] & 0xFF;
                if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
                {
                    if (i + 7 >= data.Length) return false;
                    int pIdx = i + 4;
                    if (pIdx + 4 >= data.Length) return false;
                    height = (data[pIdx + 1] << 8) | (data[pIdx + 2] & 0xFF);
                    width  = (data[pIdx + 3] << 8) | (data[pIdx + 4] & 0xFF);
                    return width > 0 && height > 0;
                }
                else
                {
                    if (i + 4 >= data.Length) return false;
                    int segLen = (data[i + 2] << 8) | (data[i + 3] & 0xFF);
                    if (segLen <= 2) return false;
                    i += 2 + segLen;
                }
            }
            return false;
        }
    }
}