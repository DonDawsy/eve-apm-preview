#ifndef BORDERSTYLE_H
#define BORDERSTYLE_H

/// Enum defining available border styles for thumbnail highlighting
enum class BorderStyle {
  Solid,          ///< Solid continuous line
  Dashed,         ///< Dashed line (animated)
  Dotted,         ///< Dotted line
  DashDot,        ///< Alternating dash and dot pattern
  FadedEdges,     ///< Border with faded opacity at edges
  CornerAccents,  ///< Decorative marks only at corners
  RoundedCorners, ///< Border with rounded corners
  Neon,           ///< Neon glow with color cycling
  Shimmer,        ///< Shimmering/sparkling effect
  ThickThin,      ///< Alternating thick and thin segments
  ElectricArc,    ///< Electric arc effect with jagged lines
  Rainbow,        ///< Rainbow color gradient cycling
  BreathingGlow,  ///< Smooth breathing glow effect
  DoubleGlow,     ///< Double layer glow effect
  Zigzag          ///< Zigzag pattern border
};

#endif // BORDERSTYLE_H
