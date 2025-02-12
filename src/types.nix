{ pkgs, serene }: import ./btree/stencil.nix {
  inherit pkgs serene;
  include-path = ./types.h;
  basetype = "struct Type *";
  branching = 32;
  cmp = "Type_cmp";
  print = "Type_print";
  treename = "Typereg";
  filename = "typereg";
}
