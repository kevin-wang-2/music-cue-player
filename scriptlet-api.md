# Scope: mcp

- [module]ibrary
    - library.***: 在script library里的module

- [module]time
    - [class]Time
        - [class_method]from_sample(sample)
        - [class_method]from_min_sec(min, sec)
        - [class_method]from_timecode(timecode, fps)
        - [class_method]from_bar_bear(bar, beat, cue) - 对指定cue的mc进行处理，注意mc是有速度轨的，如果指定cue没有mc信息则报错
        - [function]to_***

- [module]cue_list
    - [class]CueList: 用python class表示的cuelist
        - [function]list_cues: 返回[Cue]这样的列表
        - [function]insert_cue(type, cuenumber="", cuename=""): 在列表最后插入一个cue，返回被插入的cue
        - [function]insert_cue_at(ref, type, cuenumber="", cuename=""): 在ref cue后面插入一个cue，返回被插入的cue
        - [function]move_cue_at(ref, cue, to_group=False): 把一个cue移到另一个cue之后（to_group仅当移动到group cue的最后一个子cue时有作用，如果to_group=True则移到group内，不然则移到group外）
        - [function]delete_cue(cue)
    - [function]list_cue_lists: 返回[CueList]这样的列表
    - [function]insert_cue_list(name): 在列表最后插入一个cuelist，返回被插入的cuelist
    - [function]insert_cue_list_at(ref, name): 在ref后面插入cuelist，返回被插入的cuelist
    - [function]delete_cue_list(cue_list)
    - [function]get_active_cue_list()

- [module]cue
    - [class]Cue: 用python class表示的cue的抽象class（注意setter&getter）
        - 函数：select - select cue
        - 函数：go - goto cue and go
        - 函数：arm(time=None) - arm cue to position（time为mcp.time.Time对象，省略则从头arm）
        - 函数：start - dry start without selecting
        - 函数：stop - stop cue
        - 函数：disarm - disarm cue
    - [class]***Cue: 对应现在的cue类型的具体class（注意setter&getter）
    - [function]get_active_cues()
    - [function]get_current_cue()
    - [function]get_selected_cue()

- [module]event
    - [function] on_cue_fired(callback): 当有一个cue被go或者start的时候（不管prewait），给callback注入cue信息，以下类似
    - [function] on_cue_selected(callback): 当有一个cue被选择的时候
    - [function] on_cue_inserted(callback): 当有一个cue被创建的时候
    - [function] on_osc_event(callback): 当收到一个osc事件，给callback注入osc的路径
    - [function] on_midi_event(callback): 当收到一个midi事件，给callback注入midi的参数
    - [function] on_music_event(quantization, callback): quantization为"1/1", "1/2", "1/4", "1/8", "1/16"，callback里面不注入信息
    - 对应的once和unsubscribe

- [function]alert(msg): 弹出信息框
- [function]confirm(msg) → bool: 确认框，用户点Yes返回True
- [function]file(title='Open File', mode='open', filter='') → str | None: 文件框；mode="open"/"save"/"dir"；取消返回None
- [function]input(prompt, default='', title='Input') → str | None: 文本输入框；取消返回None
- [function]panic: panic
- [function]go: go当前cue
- [function]select(num): select一个cue号
- [function]get_mc: 返回{"bpm": ..., "time_signature": ..., "beat": ..., "bar": ..., "PPQ": ...}
- [function]get_state: 返回{"selected_cue": ..., "running_cues": [], "mc_master": ...}

- [module]error
    - CueNotFoundError
    - CueTypeError
    - NoMasterContextError
    - InvalidOperationError