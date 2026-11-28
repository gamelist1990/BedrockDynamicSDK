"""Usage-first excerpts for types whose declarations aren't helpful as code.

Each entry is (example shape, field rows, explanatory note). The original SDK
declarations remain the source for the ordinary method and property signatures.
"""

YES, NO, CASE = "はい", "いいえ", "条件付き"

TS_SHAPES = {
    "CommandParameter": (
        'const count: CommandParameter = {\n'
        '  name: "count", kind: "int", optional: true,\n'
        '  default: 1, min: 1, max: 10, suggestions: [1, 5, 10],\n'
        '};',
        (
            ("name", "string", YES, "引数名。ハンドラーで値を取得するキー。"),
            ("kind", "string", YES, "effect / string / message / bool / int / float / duration のいずれか。"),
            ("optional", "boolean", NO, "省略できる引数なら true。"),
            ("default", "kind による", NO, "effect・string・message は文字列、bool は真偽値、それ以外は数値。"),
            ("min / max", "number", NO, "int の下限・上限。"),
            ("suggestions", "readonly (number | string)[]", NO, "int・float の入力候補。"),
            ("infinity", "boolean", NO, "float・duration で無限値を許容する場合に true。"),
        ),
        "このオブジェクトを CommandBranch の parameters に渡します。",
    ),
    "RawMessage": (
        'const message: RawMessage = {\n  rawtext: [{ text: "Hello" }, "!"],\n};',
        (
            ("text", "string", CASE, "単一のテキストを送るとき。"),
            ("rawtext", "readonly (string | RawMessage)[]", CASE, "文字列とメッセージを並べて連結するとき。"),
        ),
        "text または rawtext のどちらかを指定します。",
    ),
    "LocaleEntries": (
        'const entries: LocaleEntries = {\n  "demo.title": "デモ",\n};',
        (
            ("翻訳キー", "string", YES, "表示先で指定するキー。"),
            ("翻訳文", "string", YES, "その言語での表示テキスト。"),
        ),
        "キーと翻訳文の辞書のほか、[key, value] または { key, value } を並べた配列も渡せます。",
    ),
    "ChoiceEntry": (
        'const choice: ChoiceEntry = { id: "normal", labelKey: "demo.normal" };',
        (
            ("id", "string", YES, "選択値。"),
            ("labelKey", "string", NO, "表示ラベルの翻訳キー。"),
            ("label", "string", NO, "直接指定する表示ラベル。"),
        ),
        "選択値の文字列だけ、または [id, labelKey] の組でも指定できます。",
    ),
    "SettingDescriptor": (
        'const control = Setting.toggle("enabled", "demo.enabled", true);\n'
        'system.settings("demo.title").add(control);',
        (
            ("kind", "string", YES, "toggle / slider / choice / text のいずれか。"),
            ("id / labelKey", "string", YES, "設定 ID と表示名の翻訳キー。"),
            ("descriptionKey", "string", NO, "説明文の翻訳キー。"),
            ("initial", "SettingValue", NO, "初期値。"),
            ("minimum / maximum / step", "number", NO, "スライダーの下限・上限・刻み幅。"),
            ("choices", "readonly ChoiceEntry[]", NO, "選択肢。"),
        ),
        "通常は Setting のファクトリーで作り、SettingsPage.add に渡します。以下は作成されるオブジェクトの項目です。",
    ),
    "EquipmentSlot": (
        "const slot: EquipmentSlot = EquipmentSlot.Mainhand;",
        (),
        "指定できる値: Mainhand / Offhand / Head / Chest / Legs / Feet。同名の EquipmentSlot のプロパティでも指定できます。",
    ),
    "EntityComponent": (
        'const equipment = entity.getComponent("minecraft:equippable");',
        (),
        "既知の ID を渡すと EntityComponentMap に対応する型になり、それ以外の文字列では対応する全コンポーネント型のいずれかを返します。未取得の場合は undefined です。",
    ),
    "EntityComponentMap": (
        "",
        (
            ("minecraft:health / health", "HealthComponent", YES, "体力。"),
            ("minecraft:food / food", "FoodComponent", YES, "満腹度。"),
            ("minecraft:player.hunger / minecraft:hunger / hunger", "FoodComponent", YES, "満腹度の別名。"),
            ("minecraft:movement / movement", "MovementComponent", YES, "速度。"),
            ("minecraft:inventory / inventory", "InventoryComponent", YES, "インベントリ。"),
            ("minecraft:equippable / equippable", "EquippableComponent", YES, "装備。"),
            ("minecraft:item / item", "ItemEntityComponent", YES, "アイテムエンティティ。"),
        ),
        "getComponent(id) に渡す既知の ID と、その戻り値の型です。",
    ),
}

CPP_SHAPES = {
    "commands": {
        "CommandParameterType": (
            "",
            (),
            "指定できる値: Integer / Message / CustomEnum / PacketEnum / String。",
        ),
        "CommandParameter": (
            'sdk::CommandParameter count{\n'
            '    .name = "count", .type = sdk::CommandParameterType::Integer,\n'
            '};',
            (
                ("name", "std::string", YES, "引数名。"),
                ("type", "CommandParameterType", NO, "引数の種類。既定値は Message。"),
                ("enumName", "std::string", CASE, "列挙型を参照するときの列挙名。"),
                ("optional", "bool", NO, "省略できる引数なら true。既定値は false。"),
            ),
            "CommandDefinition の overloads に使う引数オブジェクトです。",
        ),
        "CommandDefinition": (
            'sdk::CommandDefinition command{\n'
            '    .name = "demo", .description = "Demo", .overloads = {{count}},\n'
            '    .handler = [](const std::string& line) { /* ... */ },\n'
            '};',
            (
                ("name", "std::string", YES, "コマンド名。先頭の / は不要。"),
                ("description", "std::string", NO, "コマンドの説明。"),
                ("overloads", "引数リストの配列", YES, "各リストが一通りの引数の並び。引数なしは {{}}。"),
                ("handler", "void(const std::string&)", YES, "コマンド行を受け取る関数。"),
            ),
            "上の count を使った登録用の形です。sdk は bedrock::sdk の名前空間エイリアスです。",
        ),
    },
}
