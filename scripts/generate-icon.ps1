# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
$ErrorActionPreference = 'Stop'
$projectDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
public static class AlarmIcon {
  static GraphicsPath Rounded(float x,float y,float w,float h,float r) {
    var p=new GraphicsPath(); float d=r*2;
    p.AddArc(x,y,d,d,180,90);p.AddArc(x+w-d,y,d,d,270,90);
    p.AddArc(x+w-d,y+h-d,d,d,0,90);p.AddArc(x,y+h-d,d,d,90,90);p.CloseFigure();return p;
  }
  public static Bitmap Draw(int size) {
    int render=Math.Max(size*4,512);var high=new Bitmap(render,render,PixelFormat.Format32bppArgb);
    using(var g=Graphics.FromImage(high)) {
      g.SmoothingMode=SmoothingMode.AntiAlias;g.ScaleTransform(render/1024f,render/1024f);
      using(var tile=Rounded(42,42,940,940,222))
      using(var gradient=new LinearGradientBrush(new PointF(42,42),new PointF(982,982),Color.Cyan,Color.Blue)) {
        gradient.InterpolationColors=new ColorBlend { Colors=new[]{Color.FromArgb(103,232,249),Color.FromArgb(99,121,245),Color.FromArgb(173,88,222)},Positions=new[]{0f,.43f,1f}};
        g.FillPath(gradient,tile);
        g.SetClip(tile);
        using(var shine=new LinearGradientBrush(new RectangleF(42,42,940,550),Color.FromArgb(75,255,255,255),Color.FromArgb(0,255,255,255),90f)) g.FillEllipse(shine,-150,-150,1150,650);
        g.ResetClip();
      }
      using(var rim=Rounded(54,54,916,916,211))
      using(var edge=new LinearGradientBrush(new Point(0,54),new Point(0,970),Color.FromArgb(170,255,255,255),Color.FromArgb(50,255,255,255)))
      using(var pen=new Pen(edge,17)) g.DrawPath(pen,rim);
      for(int pass=0;pass<2;pass++) {
        g.TranslateTransform(0,pass==0?19:-19);
        using(var p=new Pen(pass==0?Color.FromArgb(56,37,59,135):Color.White,pass==0?47:43)) {
          p.StartCap=LineCap.Round;p.EndCap=LineCap.Round;
          g.DrawEllipse(p,277,302,470,470);
          g.DrawLine(p,512,537,423,399);g.DrawLine(p,512,537,658,456);
          g.DrawBezier(p,292,303,322,255,365,233,407,244);
          g.DrawBezier(p,617,244,659,233,702,255,732,303);
          g.DrawLine(p,365,746,319,809);g.DrawLine(p,659,746,705,809);
        }
      }
      using(var dot=new SolidBrush(Color.White)) g.FillEllipse(dot,483,508,58,58);
    }
    var result=new Bitmap(size,size,PixelFormat.Format32bppArgb);
    using(var g=Graphics.FromImage(result)) { g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.DrawImage(high,0,0,size,size); }
    high.Dispose();return result;
  }
  public static void Save(string directory) {
    Directory.CreateDirectory(directory); int[] sizes={16,24,32,48,64,128,256};var images=new byte[sizes.Length][];
    for(int i=0;i<sizes.Length;i++) using(var bmp=Draw(sizes[i])) using(var stream=new MemoryStream()) {bmp.Save(stream,ImageFormat.Png);images[i]=stream.ToArray();}
    using(var file=File.Create(Path.Combine(directory,"alarm.ico"))) using(var w=new BinaryWriter(file)) {
      w.Write((ushort)0);w.Write((ushort)1);w.Write((ushort)sizes.Length);int offset=6+16*sizes.Length;
      for(int i=0;i<sizes.Length;i++) {w.Write((byte)(sizes[i]==256?0:sizes[i]));w.Write((byte)(sizes[i]==256?0:sizes[i]));w.Write((byte)0);w.Write((byte)0);w.Write((ushort)1);w.Write((ushort)32);w.Write(images[i].Length);w.Write(offset);offset+=images[i].Length;}
      foreach(var png in images) w.Write(png);
    }
    using(var preview=Draw(1024)) preview.Save(Path.Combine(directory,"alarm.png"),ImageFormat.Png);
  }
}
'@
[AlarmIcon]::Save((Join-Path $projectDir 'assets'))
Write-Output 'ALARM vector-style icon: PNG and multi-resolution ICO generated.'
