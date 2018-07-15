
open Typedtree;
open SharedTypes;
open Infix;

let locStartPos = ({Location.loc_start: {pos_cnum, pos_lnum, pos_bol}}) => (
  pos_lnum,
  pos_cnum - pos_bol
);

type env = {
  stamps,
  processDoc: string => string,
  modulePath: visibilityPath,
};

let getTopDoc = structure => {
  switch structure {
  | [{str_desc: Tstr_attribute(({Asttypes.txt: "ocaml.doc" | "ocaml.text"}, PStr([{pstr_desc: Pstr_eval({pexp_desc: Pexp_constant(Const_string(doc, _))}, _)}])))}, ...rest] => (Some(doc), rest)
  | _ => (None, structure)
  };
};

let newDeclared = (~contents, ~name, ~stamp, ~modulePath, ~processDoc, exported, attributes) => {
  {
    name,
    stamp,
    deprecated: PrepareUtils.findDeprecatedAttribute(attributes),
    exported,
    modulePath,
    docstring: PrepareUtils.findDocAttribute(attributes) |?>> processDoc,
    contents,
    /* scopeType: Let, */
    /* scopeStart: env.scopeStart, */
  };
};

let addItem = (~name, ~stamp, ~env, ~contents, attributes, exported, stamps) => {
  let declared = newDeclared(~contents, ~name, ~stamp,
  ~modulePath=env.modulePath,
  ~processDoc=env.processDoc,
  !Hashtbl.mem(exported, name.txt), attributes);
  if (!Hashtbl.mem(exported, name.txt)) {
    Hashtbl.add(exported, name.txt, stamp);
  };
  Hashtbl.add(stamps, stamp, declared);
  declared;
};

let rec forSignatureType = (env, signature) => {
  open Types;
  let exported = Module.initExported();
  let topLevel = List.fold_right((item, items) => switch item {
  | Sig_value({stamp, name}, {val_type, val_kind, val_attributes, val_loc: loc}) => {
    let contents = {
      Value.recursive: false,
      typ: val_type,
    };
    let declared = addItem(~name=Location.mknoloc(name), ~stamp, ~env, ~contents, val_attributes, exported.values, env.stamps.values);
    [{...declared, contents: Module.Value(declared.contents)}, ...items]
  }
  | Sig_type({stamp, name}, {type_params, type_kind, type_attributes} as decl, _) => {
    let declared = addItem(~contents={
      Type.params: type_params |> List.map(t => (t, Location.none)),
      typ: decl,
      kind: switch type_kind {
        | Type_abstract => Abstract
        | Type_open => Open
        | Type_variant(constructors) => Variant(constructors |> List.map(({cd_id: {stamp, name}, cd_args, cd_res, cd_attributes}) => {
          Type.Constructor.stamp,
          name: Location.mknoloc(name),
          args: cd_args |> List.map(t => (t, Location.none)),
          res: cd_res,
        }))
        | Type_record(labels, _) => Record(labels |> List.map(
          ({ld_id: {stamp: astamp, name}, ld_type}) => {
            {Type.Attribute.stamp: astamp, name: Location.mknoloc(name), typ: ld_type, typLoc: Location.none}
          }
        ))
      }
    }, ~name=Location.mknoloc(name), ~stamp, ~env, type_attributes, exported.types, env.stamps.types);
    [{...declared, contents: Module.Type(declared.contents)}, ...items]
  }
  /* | Sig_module({stamp, name}, {md_type: Mty_ident(path) | Mty_alias(path), md_attributes, md_loc}, _) =>
    let declared = addItem(~contents=Module.Ident(path), ~name=Location.mknoloc(name), ~stamp, ~env, md_attributes, exported.modules, env.stamps.modules);
    [{...declared, contents: Module.Module(declared.contents)}, ...items] */
  | Sig_module({stamp, name}, {md_type, md_attributes, md_loc}, _) =>
    let declared = addItem(~contents=forModuleType(env, md_type), ~name=Location.mknoloc(name), ~stamp, ~env, md_attributes, exported.modules, env.stamps.modules);
    [{...declared, contents: Module.Module(declared.contents)}, ...items]
  | _ => items
  }, signature, []) |> List.rev;

  {Module.exported, topLevel}
} and forModuleType = (env, moduleType) => switch moduleType {
  | Types.Mty_ident(path) => Module.Ident(path)
  | Mty_alias(path) => Module.Ident(path)
  | Mty_signature(signature) => {
    Module.Structure(forSignatureType(env, signature))
  }
  | Mty_functor(argIdent, argType, resultType) => forModuleType(env, resultType)
};

let rec forItem = (
  ~env,
  ~exported: Module.exported,
  item
) => switch (item.str_desc) {
| Tstr_value(isRec, bindings) => 
  optMap(({vb_loc, vb_expr, vb_pat: {pat_desc, pat_type}, vb_attributes}) =>
    /* TODO get all the things out of the var. */
    switch (pat_desc) {
      | Tpat_var({stamp}, {txt, loc} as name) =>
        let contents = {
          Value.recursive: false,
          typ: pat_type,
        };
        let declared = addItem(~name, ~stamp, ~env, ~contents, vb_attributes, exported.values, env.stamps.values);
        Some({...declared, contents: Module.Value(declared.contents)})
      | _ => None
    },
  bindings)
| Tstr_module({mb_id: {stamp}, mb_attributes, mb_loc, mb_name: name, mb_expr: {mod_desc}}) => {
  let contents = forModule(env, mod_desc);
  let declared = addItem(~contents, ~name, ~stamp, ~env, mb_attributes, exported.modules, env.stamps.modules);
  [{...declared, contents: Module.Module(declared.contents)}]
}
| Tstr_primitive({val_id: {stamp}, val_name: name, val_attributes, val_val: {val_type}}) => {
  let declared = addItem(~contents={Value.recursive: false, typ: val_type}, ~name, ~stamp, ~env, val_attributes, exported.values, env.stamps.values);
  [{...declared, contents: Module.Value(declared.contents)}]
}
| Tstr_type(decls) => decls |> List.map(({typ_id: {stamp}, typ_params, typ_name: name, typ_attributes, typ_type, typ_kind}) => {
  let declared = addItem(~contents={
    Type.params: typ_params |> List.map(((t, _)) => (t.ctyp_type, t.ctyp_loc)),
    typ: typ_type,
    kind: switch typ_kind {
      | Ttype_abstract => Abstract
      | Ttype_open => Open
      | Ttype_variant(constructors) => Variant(constructors |> List.map(({cd_id: {stamp}, cd_name: name, cd_args, cd_res, cd_attributes}) => {
        Type.Constructor.stamp,
        name,
        args: cd_args |> List.map(t => (t.ctyp_type, t.ctyp_loc)),
        res: cd_res |?>> t => t.ctyp_type,
      }))
      | Ttype_record(labels) => Record(labels |> List.map(
        ({ld_id: {stamp: astamp}, ld_name: name, ld_type: {ctyp_type, ctyp_loc}}) => {
          {Type.Attribute.stamp: astamp, name, typ: ctyp_type, typLoc: ctyp_loc}
        }
      ))
    }
  }, ~name, ~stamp, ~env, typ_attributes, exported.types, env.stamps.types);
  {...declared, contents: Module.Type(declared.contents)}
})
  | _ => []
}

and forModule = (env, mod_desc) => switch mod_desc {
  | Tmod_ident(path, lident) => Module.Ident(path)
  | Tmod_structure(structure) => {
    let (doc, contents) = forStructure(~env, structure.str_items);
    Module.Structure(contents)
  }
  | Tmod_functor(argIdent, argName, maybeType, resultExpr) => forModule(env, resultExpr.mod_desc)
  | Tmod_apply(functor_, arg, coersion) => forModule(env, functor_.mod_desc)
  | Tmod_unpack(expr, moduleType) => forModuleType(env, moduleType)
  | Tmod_constraint(expr, typ, constraint_, coersion) => {
    /* TODO do this better I think */
    forModuleType(env, typ)
  }
}

and forStructure = (~env, items) => {
  let (doc, items) = getTopDoc(items);
  let exported = Module.initExported();
  let topLevel = List.fold_right((item, results) => {
    forItem(~env, ~exported, item) @ results
  }, items, []);
  (doc, {Module.exported, topLevel})
};

let forCmt = (uri, processDoc, {cmt_modname, cmt_annots}: Cmt_format.cmt_infos) => switch cmt_annots {
| Implementation(structure) => {
  let env = {stamps: initStamps(), processDoc, modulePath: File(uri)};
  let (docstring, contents) = forStructure(~env, structure.str_items);
  Some({uri, moduleName: cmt_modname, stamps: env.stamps, docstring, contents})
}
/* | Interface(signature) => Some(forSignature(processDoc, signature.sig_items)) */
| _ => None
};

/* let forCmi = (processDoc, {Cmi_format.cmi_name, cmi_sign}) => {
  let items = forSignatureType(processDoc, cmi_sign);
  /* TODO: want the toplevel docs... */
  Some((None, items))
}; */


