#include "hid-tmt300rs.h"

static struct t300rs_device_entry *t300rs_get_device(struct hid_device *hdev){
    struct t300rs_data *drv_data;
    struct t300rs_device_entry *t300rs;

    spin_lock_irqsave(&lock, lock_flags);
    drv_data = hid_get_drvdata(hdev);
    if(!drv_data){
        hid_err(hdev, "private data not found\n");
        return NULL;
    }

    t300rs = drv_data->device_props;
    if(!t300rs){
        hid_err(hdev, "device properties not found\n");
        return NULL;
    }
    spin_unlock_irqrestore(&lock, lock_flags);
    return t300rs;
}

static int t300rs_send_int(struct input_dev *dev, u8 *send_buffer, int *trans){
    struct hid_device *hdev = input_get_drvdata(dev);
    struct t300rs_device_entry *t300rs;
   int i;

    t300rs = t300rs_get_device(hdev);
    if(!t300rs){
        hid_err(hdev, "could not get device\n");
        return -1;
    }

    for(i = 0; i < T300RS_BUFFER_LENGTH; ++i){
        t300rs->ff_field->value[i] = send_buffer[i];
    }

    hid_hw_request(t300rs->hdev, t300rs->report, HID_REQ_SET_REPORT);

    return 0;
}

static int t300rs_play_effect(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state){
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    int ret, trans;

    
    send_buffer[1] = state->effect.id + 1;
    send_buffer[2] = 0x89;
    send_buffer[3] = 0x01;

    ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
    if(ret){
        hid_err(t300rs->hdev, "failed starting effect play\n");
    }

    kfree(send_buffer);
    return ret;
}

static int t300rs_stop_effect(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state){
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    int ret, trans;

    
    send_buffer[1] = state->effect.id + 1;
    send_buffer[2] = 0x89;

    ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
    if(ret){
        hid_err(t300rs->hdev, "failed stopping effect play\n");
    }

    kfree(send_buffer);
    return ret;
}

static void t300rs_fill_envelope(u8 *send_buffer, int i, s16 level, u16 duration, struct ff_envelope *envelope){
    u16 attack_length = (duration * envelope->attack_length) / 0x7fff;
    u16 attack_level = (level * envelope->attack_level) / 0x7fff;
    u16 fade_length = (duration * envelope->fade_length) / 0x7fff;
    u16 fade_level = (level * envelope->fade_level) / 0x7fff;

    send_buffer[i    ] = attack_length & 0xff;
    send_buffer[i + 1] = attack_length >> 8;
    send_buffer[i + 2] = attack_level & 0xff;
    send_buffer[i + 3] = attack_level >> 8;
    send_buffer[i + 4] = fade_length & 0xff;
    send_buffer[i + 5] = fade_length >> 8;
    send_buffer[i + 6] = fade_level & 0xff;
    send_buffer[i + 7] = fade_level >> 8;
}

static int t300rs_modify_envelope(struct t300rs_device_entry *t300rs,
        struct t300rs_effect_state *state,
        u8 *send_buffer,
        s16 level,
        u16 duration,
        u8 id,
        struct ff_envelope envelope,
        struct ff_envelope envelope_old
        ){
    u16 attack_length, attack_level, fade_length, fade_level;
    int ret = 0, trans;

    if(duration == 0){
        duration = 0xffff;
    }

    attack_length = (duration * envelope.attack_length) / 0x7fff;
    attack_level = (level * envelope.attack_level) / 0x7fff;
    fade_length = (duration * envelope.fade_length) / 0x7fff;
    fade_level = (level * envelope.fade_level) / 0x7fff;

    
    send_buffer[1] = id + 1;
    send_buffer[2] = 0x31;

    if(envelope.attack_length != envelope_old.attack_length){
        send_buffer[3] = 0x81;

        send_buffer[4] = attack_length & 0xff;
        send_buffer[5] = attack_length >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying effect envelope\n");
            goto error;
        }
    }

    if(envelope.attack_level != envelope_old.attack_level){
        send_buffer[3] = 0x82;

        send_buffer[4] = attack_level & 0xff;
        send_buffer[5] = attack_level >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying effect envelope\n");
            goto error;
        }
    }

    if(envelope.fade_length != envelope_old.fade_length){
        send_buffer[3] = 0x84;

        send_buffer[4] = fade_length & 0xff;
        send_buffer[5] = fade_length >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying effect envelope\n");
            goto error;
        }
    }

    if(envelope.fade_level != envelope_old.fade_level){
        send_buffer[3] = 0x88;

        send_buffer[4] = fade_level & 0xff;
        send_buffer[5] = fade_level >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying effect envelope\n");
            goto error;
        }
    }
    
error:
    memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    return ret;
}

static int t300rs_modify_duration(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state, u8 *send_buffer){
    struct ff_effect effect = state->effect;
    struct ff_effect old = state->old;
    u16 duration;
    int ret = 0, trans;
    
    if(effect.replay.length == 0){
        duration = 0xffff;
    } else {
        duration = effect.replay.length;
    }

    if(effect.replay.length != old.replay.length){
        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x49;
        send_buffer[4] = 0x41;

        send_buffer[5] = duration & 0xff;
        send_buffer[6] = duration >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying duration\n");
            goto error;
        }
    }
error:
    memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    return ret;
}

static int t300rs_modify_constant(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state, u8 *send_buffer){
    struct ff_effect effect = state->effect;
    struct ff_effect old = state->old;
    struct ff_constant_effect constant = effect.u.constant;
    struct ff_constant_effect constant_old = old.u.constant;
    int ret, trans;
    s16 level;

    level = (constant.level * fixp_sin16(effect.direction * 360 / 0x10000)) / 0x7fff;

    if(constant.level != constant_old.level){
        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0a;

        send_buffer[3] = level & 0xff;
        send_buffer[4] = level >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying constant effect\n");
            goto error;
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }

    ret = t300rs_modify_envelope(t300rs,
            state,
            send_buffer,
            level,
            effect.replay.length,
            effect.id,
            constant.envelope,
            constant_old.envelope
            );
    if(ret){
        hid_err(t300rs->hdev, "failed modifying constant envelope\n");
        goto error;
    }

    ret = t300rs_modify_duration(t300rs, state, send_buffer);
    if(ret){
        hid_err(t300rs->hdev, "failed modifying constant duration\n");
        goto error;
    }

error:
    kfree(send_buffer);
    return ret;
}

static int t300rs_modify_ramp(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state, u8 *send_buffer){
    struct ff_effect effect = state->effect;
    struct ff_effect old = state->old;
    struct ff_ramp_effect ramp = effect.u.ramp;
    struct ff_ramp_effect ramp_old = old.u.ramp;
    int ret, trans;

        u16 difference, top, bottom;
        s16 level;

        top = ramp.end_level > ramp.start_level ? ramp.end_level : ramp.start_level;
        bottom = ramp.end_level > ramp.start_level ? ramp.start_level : ramp.end_level;


        difference = ((top - bottom) * fixp_sin16(effect.direction * 360 / 0x10000)) / 0x7fff;


        level = (top * fixp_sin16(effect.direction * 360 / 0x10000)) / 0x7fff;

    if(ramp.start_level != ramp_old.start_level || ramp.end_level != ramp_old.end_level){

        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0e;
        send_buffer[3] = 0x03;

        send_buffer[4] = difference & 0xff;
        send_buffer[5] = difference >> 8;

        send_buffer[6] = level & 0xff;
        send_buffer[7] = level >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying ramp effect\n");
            goto error;
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }

    ret = t300rs_modify_envelope(t300rs,
            state,
            send_buffer,
            level,
            effect.replay.length,
            effect.id,
            ramp.envelope,
            ramp_old.envelope
            );

    if(ret){
        hid_err(t300rs->hdev, "failed modifying ramp envelope\n");
        goto error;
    }

    ret = t300rs_modify_duration(t300rs, state, send_buffer);
    if(ret){
        hid_err(t300rs->hdev, "failed modifying ramp duration\n");
        goto error;
    }

error:
    kfree(send_buffer);
    return ret;
}
static int t300rs_modify_damper(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state, u8 *send_buffer){
    struct ff_effect effect = state->effect;
    struct ff_effect old = state->old;
    struct ff_condition_effect damper = effect.u.condition[0];
    struct ff_condition_effect damper_old = old.u.condition[0];
    int ret, trans;

    if(damper.right_coeff != damper_old.right_coeff){
        s16 coeff = damper.right_coeff;

        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0e;
        send_buffer[3] = 0x41;

        send_buffer[4] = coeff & 0xff;
        send_buffer[5] = coeff >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying damper rc\n");
            goto error;
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }

    if(damper.left_coeff != damper_old.left_coeff){
        s16 coeff = damper.left_coeff;

        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0e;
        send_buffer[3] = 0x42;

        send_buffer[4] = coeff & 0xff;
        send_buffer[5] = coeff >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying damper lc\n");
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }
    
    if((damper.deadband != damper_old.deadband) ||
            (damper.center != damper_old.center)){
        u16 deadband_right = 0xfffe - damper.deadband - damper.center;
        u16 deadband_left = 0xfffe - damper.deadband + damper.center;
        
        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0e;
        send_buffer[3] = 0x4c;

        send_buffer[4] = deadband_right & 0xff;
        send_buffer[5] = deadband_right >> 8;

        send_buffer[6] = deadband_left & 0xff;
        send_buffer[7] = deadband_left >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying damper deadband\n");
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }

    ret = t300rs_modify_duration(t300rs, state, send_buffer);
    if(ret){
        hid_err(t300rs->hdev, "failed modifying damper duration\n");
        goto error;
    }

error:
    kfree(send_buffer);
    return ret;
}


static int t300rs_modify_periodic(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state, u8 *send_buffer){
    struct ff_effect effect = state->effect;
    struct ff_effect old = state->old;
    struct ff_periodic_effect periodic = effect.u.periodic;
    struct ff_periodic_effect periodic_old = old.u.periodic;
    int ret, trans;
        s16 level;

        level = (periodic.magnitude * fixp_sin16(effect.direction * 360 / 0x10000)) / 0x7fff;


    if(periodic.magnitude != periodic_old.magnitude){
        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0e;
        send_buffer[3] = 0x01;

        send_buffer[4] = level & 0xff;
        send_buffer[5] = level >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying periodic magnitude\n");
            goto error;
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }

    if(periodic.offset != periodic_old.offset){
        s16 offset = periodic.offset;

        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0e;
        send_buffer[3] = 0x02;

        send_buffer[4] = offset & 0xff;
        send_buffer[5] = offset >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying periodic offset\n");
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }
    
    if(periodic.phase != periodic_old.phase){
        s16 phase = periodic.phase;

        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0e;
        send_buffer[3] = 0x04;

        send_buffer[4] = phase & 0xff;
        send_buffer[5] = phase >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying periodic phase\n");
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }

    if(periodic.period != periodic_old.period){
        s16 period = periodic.period;

        
        send_buffer[1] = effect.id + 1;
        send_buffer[2] = 0x0e;
        send_buffer[3] = 0x08;

        send_buffer[4] = period & 0xff;
        send_buffer[5] = period >> 8;

        ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
        if(ret){
            hid_err(t300rs->hdev, "failed modifying periodic period\n");
        }

        memset(send_buffer, 0, T300RS_BUFFER_LENGTH);
    }

    ret = t300rs_modify_envelope(t300rs,
            state,
            send_buffer,
            level,
            effect.replay.length,
            effect.id,
            periodic.envelope,
            periodic_old.envelope);
    if(ret){
        hid_err(t300rs->hdev, "failed modifying periodic envelope\n");
        goto error;
    }

    ret = t300rs_modify_duration(t300rs, state, send_buffer);
    if(ret){
        hid_err(t300rs->hdev, "failed modifying periodic duration\n");
        goto error;
    }

error:
    kfree(send_buffer);
    return ret;
}


static int t300rs_upload_constant(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state){
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    struct ff_effect effect = state->effect;
    struct ff_constant_effect constant = state->effect.u.constant;
    s16 level;
    u16 duration, offset;

    int ret, trans;

    /* some games, such as DiRT Rally 2 have a weird feeling to them, sort of
     * like the wheel pulls just a bit to the right or left and then it just
     * stops. I wouldn't be surprised if it's got something to do with the
     * constant envelope, but right now I don't know. */

    if(test_bit(FF_EFFECT_PLAYING, &state->flags) && 
            test_bit(FF_EFFECT_QUEUE_UPDATE, &state->flags)){
        __clear_bit(FF_EFFECT_QUEUE_UPLOAD, &state->flags);
        return t300rs_modify_constant(t300rs, state, send_buffer);
    }

    level = (constant.level * fixp_sin16(effect.direction * 360 / 0x10000)) / 0x7fff;
    if(effect.replay.length == 0){
        duration = 0xffff;
    } else {
        duration = effect.replay.length;
    }

    offset = effect.replay.delay;

    
    send_buffer[1] = effect.id + 1;
    send_buffer[2] = 0x6a;

    send_buffer[3] = level & 0xff;
    send_buffer[4] = level >> 8;

    t300rs_fill_envelope(send_buffer, 5, level,
            duration, &constant.envelope);

    send_buffer[14] = 0x4f;

    send_buffer[15] = duration & 0xff;
    send_buffer[16] = duration >> 8;

    send_buffer[19] = offset & 0xff;
    send_buffer[20] = offset >> 8;

    send_buffer[22] = 0xff;
    send_buffer[23] = 0xff;

    ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
    if(ret){
        hid_err(t300rs->hdev, "failed uploading constant effect\n");
    }

    kfree(send_buffer);
    return ret;
}

static int t300rs_upload_ramp(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state){
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    struct ff_effect effect = state->effect;
    struct ff_ramp_effect ramp = state->effect.u.ramp;
    int ret, trans;
    u16 difference, offset, top, bottom, duration;
    s16 level;

    if(test_bit(FF_EFFECT_PLAYING, &state->flags) &&
            test_bit(FF_EFFECT_QUEUE_UPDATE, &state->flags)){
        __clear_bit(FF_EFFECT_QUEUE_UPLOAD, &state->flags);
        return t300rs_modify_ramp(t300rs, state, send_buffer);
    }

    if(effect.replay.length == 0){
        duration = 0xffff;
    } else {
        duration = effect.replay.length;
    }

    top = ramp.end_level > ramp.start_level ? ramp.end_level : ramp.start_level;
    bottom = ramp.end_level > ramp.start_level ? ramp.start_level : ramp.end_level;


    difference = ((top - bottom) * fixp_sin16(effect.direction * 360 / 0x10000)) / 0x7fff;
    level = (top * fixp_sin16(effect.direction * 360 / 0x10000)) / 0x7fff;
    offset = effect.replay.delay;

    
    send_buffer[1] = effect.id + 1;
    send_buffer[2] = 0x6b;
    
    send_buffer[3] = difference & 0xff;
    send_buffer[4] = difference >> 8;

    send_buffer[5] = level & 0xff; 
    send_buffer[6] = level >> 8;

    send_buffer[9] = duration & 0xff;
    send_buffer[10] = duration >> 8;

    send_buffer[12] = 0x80;

    t300rs_fill_envelope(send_buffer, 13, level,
            effect.replay.length, &ramp.envelope);

    send_buffer[22] = ramp.end_level > ramp.start_level ? 0x04 : 0x05;
    send_buffer[23] = 0x4f;

    send_buffer[24] = duration & 0xff;
    send_buffer[25] = duration >> 8;

    send_buffer[28] = offset & 0xff;
    send_buffer[29] = offset >> 8;

    send_buffer[31] = 0xff;
    send_buffer[32] = 0xff;

    ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
    if(ret){
        hid_err(t300rs->hdev, "failed uploading ramp");
    }

    kfree(send_buffer);
    return ret;
}

static int t300rs_upload_spring(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state){
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    struct ff_effect effect = state->effect;
    /* we only care about the first axis */
    struct ff_condition_effect spring = state->effect.u.condition[0];
    int ret, trans;
    u16 duration, right_coeff, left_coeff, deadband_right, deadband_left, offset;
    
    if(test_bit(FF_EFFECT_PLAYING, &state->flags) && 
            test_bit(FF_EFFECT_QUEUE_UPDATE, &state->flags)){
        __clear_bit(FF_EFFECT_QUEUE_UPLOAD, &state->flags);
        return t300rs_modify_damper(t300rs, state, send_buffer);
    }

    if(effect.replay.length == 0){
        duration = 0xffff;
    } else {
        duration = effect.replay.length;
    }

    
    send_buffer[1] = effect.id + 1;
    send_buffer[2] = 0x64;
    
    right_coeff = spring.right_coeff;
    left_coeff = spring.left_coeff;

    deadband_right = 0xfffe - spring.deadband - spring.center;
    deadband_left = 0xfffe - spring.deadband + spring.center;

    offset = effect.replay.delay;

    send_buffer[3] = right_coeff & 0xff;
    send_buffer[4] = right_coeff >> 8;

    send_buffer[5] = left_coeff & 0xff;
    send_buffer[6] = left_coeff >> 8;

    send_buffer[7] = deadband_right & 0xff;
    send_buffer[8] = deadband_right >> 8;

    send_buffer[9] = deadband_left & 0xff;
    send_buffer[10] = deadband_left >> 8;

    memcpy(&send_buffer[11], spring_values, ARRAY_SIZE(spring_values));
    send_buffer[28] = 0x4f;

    send_buffer[29] = duration & 0xff;
    send_buffer[30] = duration >> 8;

    send_buffer[33] = offset & 0xff;
    send_buffer[34] = offset >> 8;

    send_buffer[36] = 0xff;
    send_buffer[37] = 0xff;

    ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
    if(ret){
        hid_err(t300rs->hdev, "failed uploading spring\n");
    }

    kfree(send_buffer);
    return ret;
}

static int t300rs_upload_damper(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state){
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    struct ff_effect effect = state->effect;
    /* we only care about the first axis */
    struct ff_condition_effect spring = state->effect.u.condition[0];
    int ret, trans;
    u16 duration, right_coeff, left_coeff, deadband_right, deadband_left, offset;
    
    if(test_bit(FF_EFFECT_PLAYING, &state->flags) && 
            test_bit(FF_EFFECT_QUEUE_UPDATE, &state->flags)){
        __clear_bit(FF_EFFECT_QUEUE_UPLOAD, &state->flags);
        return t300rs_modify_damper(t300rs, state, send_buffer);
    }

    if(effect.replay.length == 0){
        duration = 0xffff;
    } else {
        duration = effect.replay.length;
    }

    
    send_buffer[1] = effect.id + 1;
    send_buffer[2] = 0x64;
    
    right_coeff = spring.right_coeff;
    left_coeff = spring.left_coeff;

    deadband_right = 0xfffe - spring.deadband - spring.center;
    deadband_left = 0xfffe - spring.deadband + spring.center;

    offset = effect.replay.delay;

    send_buffer[3] = right_coeff & 0xff;
    send_buffer[4] = right_coeff >> 8;

    send_buffer[5] = left_coeff & 0xff;
    send_buffer[6] = left_coeff >> 8;

    send_buffer[7] = deadband_right & 0xff;
    send_buffer[8] = deadband_right >> 8;

    send_buffer[9] = deadband_left & 0xff;
    send_buffer[10] = deadband_left >> 8;

    memcpy(&send_buffer[11], damper_values, ARRAY_SIZE(damper_values));
    send_buffer[28] = 0x4f;

    send_buffer[29] = duration & 0xff;
    send_buffer[30] = duration >> 8;

    send_buffer[33] = offset & 0xff;
    send_buffer[34] = offset >> 8;

    send_buffer[36] = 0xff;
    send_buffer[37] = 0xff;

    ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
    if(ret){
        hid_err(t300rs->hdev, "failed uploading spring\n");
    }

    kfree(send_buffer);
    return ret;
}

static int t300rs_upload_periodic(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state){
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    struct ff_effect effect = state->effect;
    struct ff_periodic_effect periodic = state->effect.u.periodic;
    int ret, trans;
    u16 duration, magnitude, phase, period, offset;
    s16 periodic_offset;

    if(test_bit(FF_EFFECT_PLAYING, &state->flags) && 
            test_bit(FF_EFFECT_QUEUE_UPDATE, &state->flags)){
        __clear_bit(FF_EFFECT_QUEUE_UPLOAD, &state->flags);
        return t300rs_modify_periodic(t300rs, state, send_buffer);
    }

    if(effect.replay.length == 0){
        duration = 0xffff;
    } else {
        duration = effect.replay.length;
    }

    magnitude = (periodic.magnitude * fixp_sin16(effect.direction * 360 / 0x10000)) / 0x7fff;

    phase = periodic.phase;
    periodic_offset = periodic.offset;
    period = periodic.period;
    offset = effect.replay.delay;
    
    
    send_buffer[1] = effect.id + 1;
    send_buffer[2] = 0x6b;
    
    send_buffer[3] = magnitude & 0xff;
    send_buffer[4] = magnitude >> 8;

    send_buffer[7] = phase & 0xff;
    send_buffer[8] = phase >> 8;

    send_buffer[5] = periodic_offset & 0xff;
    send_buffer[6] = periodic_offset >> 8;

    send_buffer[9] = period & 0xff;
    send_buffer[10] = period >> 8;

    send_buffer[12] = 0x80;

    t300rs_fill_envelope(send_buffer, 13, magnitude,
            effect.replay.length, &periodic.envelope);

    send_buffer[21] = periodic.waveform - 0x57;
    send_buffer[22] = 0x4f;

    send_buffer[23] = duration & 0xff;
    send_buffer[24] = duration >> 8;

    send_buffer[27] = offset & 0xff;
    send_buffer[28] = offset >> 8;

    send_buffer[30] = 0xff;
    send_buffer[31] = 0xff;
    
    ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
    if(ret){
        hid_err(t300rs->hdev, "failed uploading periodic effect");
    }
     
    kfree(send_buffer);
    return ret;
}

static int t300rs_upload_effect(struct t300rs_device_entry *t300rs, struct t300rs_effect_state *state){
    switch(state->effect.type){
        case FF_CONSTANT:
            return t300rs_upload_constant(t300rs, state);
        case FF_RAMP:
            return t300rs_upload_ramp(t300rs, state);
        case FF_SPRING:
            return t300rs_upload_spring(t300rs, state);
        case FF_DAMPER:
        case FF_FRICTION:
        case FF_INERTIA:
            return t300rs_upload_damper(t300rs, state);
        case FF_PERIODIC:
            return t300rs_upload_periodic(t300rs, state);
        default:
            hid_err(t300rs->hdev, "invalid effect type: %x", state->effect.type);
            return -1;
    }
} 

static int t300rs_timer_helper(struct t300rs_device_entry *t300rs){
    struct t300rs_effect_state *state;
    unsigned long jiffies_now = JIFFIES2MS(jiffies);
    int max_count = 0, effect_id, ret;
    
    for(effect_id = 0; effect_id < T300RS_MAX_EFFECTS; ++effect_id){
        
        state = &t300rs->states[effect_id];

         if(test_bit(FF_EFFECT_PLAYING, &state->flags) && state->effect.replay.length){
             if((jiffies_now - state->start_time) >= state->effect.replay.length){
                __clear_bit(FF_EFFECT_PLAYING, &state->flags);

                /* lazy bum fix? */
                __clear_bit(FF_EFFECT_QUEUE_UPDATE, &state->flags);

                if(state->count){
                    state->count--;
                }

                if(state->count){
                    __set_bit(FF_EFFECT_QUEUE_START, &state->flags);
                }
            }
        }

        if(test_bit(FF_EFFECT_QUEUE_UPLOAD, &state->flags)){
            __clear_bit(FF_EFFECT_QUEUE_UPLOAD, &state->flags);

            ret = t300rs_upload_effect(t300rs, state);
            if(ret){
                hid_err(t300rs->hdev, "failed uploading effects");
                return ret;
            }
        }

        if(test_bit(FF_EFFECT_QUEUE_START, &state->flags)){
            __clear_bit(FF_EFFECT_QUEUE_START, &state->flags);
            __set_bit(FF_EFFECT_PLAYING, &state->flags);

            ret = t300rs_play_effect(t300rs, state);
            if(ret){
                hid_err(t300rs->hdev, "failed starting effects\n");
                return ret;
            }

        }

        if(test_bit(FF_EFFECT_QUEUE_STOP, &state->flags)){
            __clear_bit(FF_EFFECT_QUEUE_STOP, &state->flags);
            __clear_bit(FF_EFFECT_PLAYING, &state->flags);

            ret = t300rs_stop_effect(t300rs, state);
            if(ret){
                hid_err(t300rs->hdev, "failed stopping effect\n");
                return ret;
            }
        }

        if(state->count > max_count){
            max_count = state->count;
        }
    }

    return max_count;
}

static enum hrtimer_restart t300rs_timer(struct hrtimer *t){
    struct t300rs_device_entry *t300rs = container_of(t, struct t300rs_device_entry, hrtimer);
    int max_count;

    max_count = t300rs_timer_helper(t300rs);
    
    if(max_count > 0){
        hrtimer_forward_now(&t300rs->hrtimer, ms_to_ktime(timer_msecs));
        return HRTIMER_RESTART;
    } else {
        return HRTIMER_NORESTART;
    }
} 

static int t300rs_upload(struct input_dev *dev, struct ff_effect *effect, struct ff_effect *old){
    struct hid_device *hdev = input_get_drvdata(dev);
    struct t300rs_device_entry *t300rs;
    struct t300rs_effect_state *state;

    t300rs = t300rs_get_device(hdev);
    
    if(effect->type == FF_PERIODIC && effect->u.periodic.period == 0){
        return -EINVAL;
    }

    if(effect->id > t300rs->max_id){
        t300rs->max_id = effect->id;
    }

    state = &t300rs->states[effect->id];

    spin_lock_irqsave(&t300rs->lock, t300rs->lock_flags);

    state->effect = *effect;
    if(old){
        state->old = *old;
        __set_bit(FF_EFFECT_QUEUE_UPDATE, &state->flags);
    } else {
        __clear_bit(FF_EFFECT_QUEUE_UPDATE, &state->flags);
    }
    __set_bit(FF_EFFECT_QUEUE_UPLOAD, &state->flags);

    spin_unlock_irqrestore(&t300rs->lock, t300rs->lock_flags);

    return 0;
}

static int t300rs_play(struct input_dev *dev, int effect_id, int value){
    struct hid_device *hdev = input_get_drvdata(dev);
    struct t300rs_device_entry *t300rs;
    struct t300rs_effect_state *state;

    t300rs = t300rs_get_device(hdev);

    state = &t300rs->states[effect_id];

    if(&state->effect == 0)
        return 0;
    
    spin_lock_irqsave(&t300rs->lock, t300rs->lock_flags);

    if(value > 0){
        state->count = value;
        state->start_time = JIFFIES2MS(jiffies);
        __set_bit(FF_EFFECT_QUEUE_START, &state->flags);

        if(test_bit(FF_EFFECT_QUEUE_STOP, &state->flags))
            __clear_bit(FF_EFFECT_QUEUE_STOP, &state->flags);

    } else {
            __set_bit(FF_EFFECT_QUEUE_STOP, &state->flags);
    }

    if(!hrtimer_active(&t300rs->hrtimer)){
        hrtimer_start(&t300rs->hrtimer, ms_to_ktime(timer_msecs), HRTIMER_MODE_REL);
    }

    spin_unlock_irqrestore(&t300rs->lock, t300rs->lock_flags);
    return 0;
}

/* we should set a default range */
static ssize_t t300rs_range_store(struct device *dev, struct device_attribute *attr,
        const char *buf, size_t count){
    struct hid_device *hdev = to_hid_device(dev);
    struct t300rs_device_entry *t300rs;
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    u16 range = simple_strtoul(buf, NULL, 10);
    int ret, trans;

    t300rs = t300rs_get_device(hdev);

    if(range < 40){
        range = 40;
    }

    if(range > 1080){
        range = 1080;
    }

    range *= 0x3c;

    
    send_buffer[0] = 0x08;
    send_buffer[1] = 0x11;
    send_buffer[2] = range & 0xff;
    send_buffer[3] = range >> 8;

    ret = t300rs_send_int(t300rs->input_dev, send_buffer, &trans);
    if(ret){
        hid_err(hdev, "failed sending interrupts\n");
        return -1;
    }

    t300rs->range = range / 0x3c;
    kfree(send_buffer);
    return count;
}

static ssize_t t300rs_range_show(struct device *dev, struct device_attribute *attr,
        char *buf){
    struct hid_device *hdev = to_hid_device(dev);
    struct t300rs_device_entry *t300rs;
    size_t count;

    t300rs = t300rs_get_device(hdev);
    count = scnprintf(buf, PAGE_SIZE, "%u\n", t300rs->range);
    return count;
}

static DEVICE_ATTR(range, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, t300rs_range_show, t300rs_range_store);

static void t300rs_set_autocenter(struct input_dev *dev, u16 value){
    struct hid_device *hdev = input_get_drvdata(dev);
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    int ret, trans;

    
    send_buffer[0] = 0x08;
    send_buffer[1] = 0x04;
    send_buffer[2] = 0x01;
    
    ret = t300rs_send_int(dev, send_buffer, &trans);
    if(ret){
        hid_err(hdev, "failed setting autocenter");
    }

    send_buffer[0] = 0x08;
    send_buffer[1] = 0x03;
    
    send_buffer[2] = value & 0xff;
    send_buffer[3] = value >> 8;

    ret = t300rs_send_int(dev, send_buffer, &trans);
    if(ret){
        hid_err(hdev, "failed setting autocenter");
    }

    kfree(send_buffer);
}

static void t300rs_set_gain(struct input_dev *dev, u16 gain){
    struct hid_device *hdev = input_get_drvdata(dev);
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    int ret, trans;

    
    send_buffer[0] = 0x02;
    send_buffer[1] = SCALE_VALUE_U16(gain, 8);
    
    ret = t300rs_send_int(dev, send_buffer, &trans);
    if(ret){
        hid_err(hdev, "failed setting gain: %i\n", ret);
    }

    kfree(send_buffer);
}

static void t300rs_destroy(struct ff_device *ff){
    /* maybe not necessary? */
    return;
}

static int t300rs_open(struct input_dev *dev){
    struct t300rs_device_entry *t300rs;
    struct hid_device *hdev = input_get_drvdata(dev);
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    int ret, trans;
    
    t300rs = t300rs_get_device(hdev);

    send_buffer[0] = 0x01;
    send_buffer[1] = 0x05;

    ret = t300rs_send_int(dev, send_buffer, &trans); 
    if(ret){
        hid_err(hdev, "failed sending interrupts\n");
        goto err;
    }
    
err:

    kfree(send_buffer);
    return t300rs->open(dev);
}

static void t300rs_close(struct input_dev *dev){
    int ret, trans;
    struct hid_device *hdev = input_get_drvdata(dev);
    struct t300rs_device_entry *t300rs;
    u8 *send_buffer = kzalloc(T300RS_BUFFER_LENGTH, GFP_ATOMIC);
    
    t300rs = t300rs_get_device(hdev);

    send_buffer[0] = 0x01;

    ret = t300rs_send_int(dev, send_buffer, &trans);
    if(ret){
        hid_err(hdev, "failed sending interrupts\n");
        goto err;
    }
err:
    kfree(send_buffer);
    t300rs->close(dev);
    return;
}

int t300rs_init(struct hid_device *hdev, const signed short *ff_bits){
    struct t300rs_device_entry *t300rs;
    struct t300rs_data *drv_data;
    struct list_head *report_list;
    struct hid_input *hidinput = list_entry(hdev->inputs.next,
            struct hid_input, list);
    struct input_dev *input_dev = hidinput->input;
    struct device *dev = &hdev->dev;
    struct usb_interface *usbif = to_usb_interface(dev->parent);
    struct usb_device *usbdev = interface_to_usbdev(usbif);
    struct hid_report *report;
    struct ff_device *ff;
    u8 *transfer = kzalloc(64, GFP_ATOMIC);
    char range[10] = "900"; /* max */
    int i, ret;

    drv_data = hid_get_drvdata(hdev);
    if(!drv_data){
        hid_err(hdev, "private driver data not allocated\n");
        ret = -ENOMEM;
        goto err;
    }

    t300rs = kzalloc(sizeof(struct t300rs_device_entry), GFP_ATOMIC);
    if(!t300rs){
        hid_err(hdev, "device entry could not be created\n");
        ret = -ENOMEM;
        goto t300rs_err;
    }

    t300rs->input_dev = input_dev;
    t300rs->hdev = hdev;
    t300rs->usbdev = usbdev;
    t300rs->usbif = usbif;
    t300rs->max_id = 0;
    t300rs->states = kmalloc(sizeof(struct t300rs_effect_state) * 0x60, GFP_ATOMIC);

    if(!t300rs->states){
        hid_err(hdev, "effect states could not be created\n");
        ret = -ENOMEM;
        goto states_err;
    }

    spin_lock_init(&t300rs->lock);

    drv_data->device_props = t300rs;

    report_list = &hdev->report_enum[HID_OUTPUT_REPORT].report_list;
    list_for_each_entry(report, report_list, list){
        int fieldnum;

        for(fieldnum = 0; fieldnum < report->maxfield; ++fieldnum){
            struct hid_field *field = report->field[fieldnum];

            if(field->maxusage <= 0){
                continue;
            }

            switch(field->usage[0].hid){
                case 0xff000060:
                    if(field->report_count < 2){
                        hid_warn(hdev, "ignoring FF field with report_count < 2\n");
                        continue;
                    }

                    if(field->logical_maximum == field->logical_minimum){
                        hid_warn(hdev, "ignoring FF field with l_max == l_min");
                        continue;
                    }

                    if(t300rs->report && t300rs->report != report){
                        hid_warn(hdev, "ignoring FF field in other report\n");
                        continue;
                    }

                    if(t300rs->ff_field && t300rs->ff_field != field){
                        hid_warn(hdev, "ignoring duplicate FF field\n");
                        continue;
                    }

                    t300rs->report = report;
                    t300rs->ff_field = field;

                    for(i = 0; ff_bits[i] >= 0; ++i){
                        set_bit(ff_bits[i], input_dev->ffbit);
                    }

                    break;

                default:
                    hid_warn(hdev, "ignoring unknown output usage: %i\n", field->usage[0].hid);
                    continue;
            }
        }
    }

    if(!t300rs->report){
        hid_err(hdev, "can't find FF field in output reports\n");
        ret = -ENODEV;
        goto out;
    }

    ret = input_ff_create(input_dev, T300RS_MAX_EFFECTS);
    if(ret){
        hid_err(hdev, "could not create input_ff\n");
        goto out;
    }

    ff = input_dev->ff;
    ff->upload = t300rs_upload;
    ff->playback = t300rs_play;
    ff->set_gain = t300rs_set_gain;
    ff->set_autocenter = t300rs_set_autocenter;
    ff->destroy = t300rs_destroy;

    t300rs->open = input_dev->open;
    t300rs->close = input_dev->close;

    input_dev->open = t300rs_open;
    input_dev->close = t300rs_close;

    ret = device_create_file(&hdev->dev, &dev_attr_range);
    if(ret){
        hid_warn(hdev, "unable to create sysfs interface for range\n");
        goto out;
    }

    hrtimer_init(&t300rs->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    t300rs->hrtimer.function = t300rs_timer;
    
    t300rs_range_store(dev, &dev_attr_range, range, 10);
    t300rs_set_gain(input_dev, 0xffff);

    kfree(transfer);

    hid_info(hdev, "force feedback for T300RS\n");
    return 0;

out:
    kfree(t300rs->states);
states_err:
    kfree(t300rs);
t300rs_err:
    kfree(drv_data);
err:
    hid_err(hdev, "failed creating force feedback device\n");
    return ret;

}

static int t300rs_probe(struct hid_device *hdev, const struct hid_device_id *id){
    int ret;
    struct t300rs_data *drv_data;

    spin_lock_init(&lock);

    drv_data = kzalloc(sizeof(struct t300rs_data), GFP_ATOMIC);
    if(!drv_data){
        hid_err(hdev, "out of memory\n");
        ret = -ENOMEM;
        goto err;
    }

    drv_data->quirks = id->driver_data;
    hid_set_drvdata(hdev, (void*)drv_data);

    ret = hid_parse(hdev);
    if(ret){
        hid_err(hdev, "parse failed\n");
        goto err;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
    if(ret){
        hid_err(hdev, "hw start failed\n");
        goto err;
    }

    ret = t300rs_init(hdev, (void*)id->driver_data);
    if(ret){
        hid_err(hdev, "t300rs_init failed\n");
        goto err;
    }
    return 0;
err:
    kfree(drv_data);
    return ret;
}

static void t300rs_remove(struct hid_device *hdev){
    struct t300rs_device_entry *t300rs;
    struct t300rs_data *drv_data;

    device_remove_file(&hdev->dev, &dev_attr_range);

    drv_data = hid_get_drvdata(hdev);
    t300rs = t300rs_get_device(hdev);

    hrtimer_cancel(&t300rs->hrtimer);

    hid_hw_stop(hdev);
    kfree(t300rs->states);
    kfree(drv_data);
    kfree(t300rs);

    return;
}

static __u8 *t300rs_report_fixup(struct hid_device *hdev, __u8 *rdesc, unsigned int *rsize){
    rdesc = t300rs_rdesc_fixed;
    *rsize = sizeof(t300rs_rdesc_fixed);
    return rdesc;
}

static const struct hid_device_id t300rs_devices[] = {
    {HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb66e),
        .driver_data = (unsigned long)t300rs_ff_effects},
    {}
};
MODULE_DEVICE_TABLE(hid, t300rs_devices);

static struct hid_driver t300rs_driver = {
    .name = "t300rs",
    .id_table = t300rs_devices,
    .probe = t300rs_probe,
    .remove = t300rs_remove,
    .report_fixup = t300rs_report_fixup,
};
module_hid_driver(t300rs_driver);

MODULE_LICENSE("GPL");
