/* Copyright (C) 2018-2019 Thomas Jespersen, TKJ Electronics. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the MIT License
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the MIT License for further details.
 *
 * Contact information
 * ------------------------------------------
 * Thomas Jespersen, TKJ Electronics
 * Web      :  http://www.tkjelectronics.dk
 * e-mail   :  thomasj@tkjelectronics.dk
 * ------------------------------------------
 */
 
#include "ADC.h"

#include "Debug.h"
#include <string.h> // for memset

ADC::hardware_resource_t * ADC::resADC1 = 0;
float ADC::ADC_REF_CORR = 1.0f;

// Necessary to export for compiler to generate code to be called by interrupt vector
extern "C" __EXPORT void ADC1_COMP_IRQHandler(void);
extern "C" __EXPORT void DMA1_Channel1_IRQHandler(void);

ADC::ADC(adc_t adc, adc_channel_t channel, uint32_t resolution) : _channel(channel)
{
	InitPeripheral(adc, resolution);
}
ADC::ADC(adc_t adc, adc_channel_t channel) : _channel(channel)
{
	InitPeripheral(adc, ADC_DEFAULT_RESOLUTION);
}

ADC::~ADC()
{
	if (!_hRes) return;
	_hRes->map_channel2bufferIndex[_channel] = 0xFF; // mark as unconfigured
	_hRes->numberOfConfiguredChannels--;

	// Stop ADC
	// ToDo: Stop ADC+DMA here

	ERROR("Deinit (destruction) of ADC with DMA enabled has not been implemented yet!");
	return;

	// Missing deinit of GPIO, eg. HAL_GPIO_DeInit(GPIOF, GPIO_PIN_3)

	if (_hRes->numberOfConfiguredChannels == 0) { // no more channels in use in resource, so delete the resource
		// Delete hardware resource
		timer_t tmpADC = _hRes->adc;
		delete(_hRes);

		switch (tmpADC)
		{
			case ADC_1:
				// ToDo: Deinit ADC here
				resADC1 = 0;
				break;
			default:
				ERROR("Undefined ADC");
				return;
		}
	}
}

void ADC::InitPeripheral(adc_t adc, uint32_t resolution)
{
	bool configureResource = false;

	_hRes = 0;

	switch (adc)
	{
		case ADC_1:
			if (!resADC1) {
				resADC1 = new ADC::hardware_resource_t;
				memset(resADC1, 0, sizeof(ADC::hardware_resource_t));
				configureResource = true;
				_hRes = resADC1;
			}
			else {
				_hRes = resADC1;
			}
			break;
		default:
			ERROR("Undefined ADC");
			return;
	}

	if (configureResource) { // first time configuring peripheral
		_hRes->adc = adc;
		_hRes->resolution = resolution;
		_hRes->numberOfConfiguredChannels = 0;
		memset(_hRes->map_channel2bufferIndex, 0xFF, sizeof(_hRes->map_channel2bufferIndex));
		memset(_hRes->buffer, 0, sizeof(_hRes->buffer));

		if (adc == ADC_1) {
			// configure VREFINT channel as the first one, such that this channel is always read
			_hRes->map_channel2bufferIndex[ADC_CHANNEL_VREFINT] = 0;
			_hRes->numberOfConfiguredChannels = 1;
		}
	}

	// Ensure that the channel is valid and not already in use
	if (_hRes->map_channel2bufferIndex[_channel] != 0xFF) {
		_hRes = 0;
		ERROR("Channel already configured on selected ADC");
		return;
	}

	// ToDo: Recompute map_channel2bufferIndex

	if (_hRes->resolution != resolution) {
		_hRes = 0;
		ERROR("ADC already in used with different resolution");
		return;
	}

	ConfigureADCGPIO();
	ConfigureADCPeripheral();
	//ConfigureADCChannels();
}

void ADC::ConfigureADCPeripheral()
{
	if (!_hRes) return;

	//ConfigureDMA();

	/* ADC Periph interface clock configuration */
	if (_hRes->adc == ADC_1) {
		LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_ADC1);
		_hRes->instance = ADC1;
		NVIC_SetPriority(ADC1_IRQn, 0); // ADC IRQ needs a greater priority than DMA IRQ
		NVIC_EnableIRQ(ADC1_IRQn);
	}

	// Can only configure ADC if it is not already running
	if(__LL_ADC_IS_ENABLED_ALL_COMMON_INSTANCE() == 0)
	{
		/* Enable internal channels */
		LL_ADC_SetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(_hRes->instance), (LL_ADC_PATH_INTERNAL_VREFINT | LL_ADC_PATH_INTERNAL_TEMPSENSOR));

		/*wait_loop_index = ((LL_ADC_DELAY_TEMPSENSOR_STAB_US * (SystemCoreClock / (100000 * 2))) / 10);
		while(wait_loop_index != 0)
		{
		wait_loop_index--;
		}*/
		HAL_Delay(1);
	}

	// Stop ADC to be able to configure channel
	// ToDo: Stop DMA
	// ToDo: Deinit ADC

	// Configure ADC
	if (LL_ADC_IsEnabled(_hRes->instance) == 0)
	{
		/* Set ADC clock (conversion clock) */
		LL_ADC_SetClock(_hRes->instance, LL_ADC_CLOCK_SYNC_PCLK_DIV2);

		/* Set ADC data resolution */
		if (_hRes->resolution == LL_ADC_RESOLUTION_8B) {
			_hRes->range = ((uint32_t)1 << 8) - 1; /* 8-bit resolution for converted data */
		} else if (_hRes->resolution == LL_ADC_RESOLUTION_10B) {
			_hRes->range = ((uint32_t)1 << 10) - 1; /* 10-bit resolution for converted data */
		} else if (_hRes->resolution == LL_ADC_RESOLUTION_12B) {
			_hRes->range = ((uint32_t)1 << 12) - 1; /* 12-bit resolution for converted data */
		} else {
			_hRes = 0;
			ERROR("Incorrect ADC resolution");
			return;
		}
		LL_ADC_SetResolution(_hRes->instance, _hRes->resolution);

		/* Set ADC conversion data alignment */
		//LL_ADC_SetResolution(_hRes->instance, LL_ADC_DATA_ALIGN_RIGHT);

		/* Set ADC low power mode */
		//LL_ADC_SetLowPowerMode(_hRes->instance, LL_ADC_LP_MODE_NONE);

		/* Set ADC channels sampling time (is configured common to all channels) */
		LL_ADC_SetSamplingTimeCommonChannels(_hRes->instance, LL_ADC_SAMPLINGTIME_239CYCLES_5);
	}

	// Configure ADC trigger and sampling
	if ((LL_ADC_IsEnabled(_hRes->instance) == 0) || (LL_ADC_REG_IsConversionOngoing(_hRes->instance) == 0))
	{
		/* Set ADC group regular trigger source */
		LL_ADC_REG_SetTriggerSource(_hRes->instance, LL_ADC_REG_TRIG_SOFTWARE);

		/* Set ADC group regular trigger polarity */
		//LL_ADC_REG_SetTriggerEdge(_hRes->instance, LL_ADC_REG_TRIG_EXT_RISING);

		/* Set ADC group regular continuous mode */
		LL_ADC_REG_SetContinuousMode(_hRes->instance, LL_ADC_REG_CONV_CONTINUOUS);

		/* Set ADC group regular conversion data transfer */
		LL_ADC_REG_SetDMATransfer(_hRes->instance, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);

		/* Set ADC group regular overrun behavior */
		LL_ADC_REG_SetOverrun(_hRes->instance, LL_ADC_REG_OVR_DATA_OVERWRITTEN);

		/* Set ADC group regular sequencer discontinuous mode */
		//LL_ADC_REG_SetSequencerDiscont(_hRes->instance, LL_ADC_REG_SEQ_DISCONT_DISABLE);

		/* Configure ADC channels */
		LL_ADC_REG_SetSequencerChannels(_hRes->instance, LL_ADC_CHANNEL_2 | LL_ADC_CHANNEL_VREFINT | LL_ADC_CHANNEL_TEMPSENSOR);
		// ADC Channel ADC_CHANNEL_TEMPSENSOR is on ADC channel 16
		// ADC Channel ADC_CHANNEL_VREFINT is on ADC channel 17
	}

	LL_ADC_EnableIT_EOC(_hRes->instance);

	/* Enable interruption ADC group regular end of sequence conversions */
	LL_ADC_EnableIT_EOS(_hRes->instance);

	/* Enable interruption ADC group regular overrun */
	LL_ADC_EnableIT_OVR(_hRes->instance);

	_hRes->map_channel2bufferIndex[ADC_CHANNEL_VREFINT] = 0;
	_hRes->map_channel2bufferIndex[ADC_CHANNEL_2] = 1;
	_hRes->map_channel2bufferIndex[ADC_CHANNEL_TEMPSENSOR] = 2;
	_hRes->numberOfConfiguredChannels = 3;

	// Calibrate and Enable ADC
	__IO uint32_t backup_setting_adc_dma_transfer = 0;
	if (LL_ADC_IsEnabled(_hRes->instance) == 0)
	{
		/* Disable ADC DMA transfer request during calibration */
		//backup_setting_adc_dma_transfer = LL_ADC_REG_GetDMATransfer(_hRes->instance);
		//LL_ADC_REG_SetDMATransfer(_hRes->instance, LL_ADC_REG_DMA_TRANSFER_NONE);

		/* Run ADC self calibration */
		LL_ADC_StartCalibration(_hRes->instance);

		/* Poll for ADC effectively calibrated */
		#if (USE_TIMEOUT == 1)
		Timeout = ADC_CALIBRATION_TIMEOUT_MS;
		#endif /* USE_TIMEOUT */

		while (LL_ADC_IsCalibrationOnGoing(_hRes->instance) != 0)
		{
			#if (USE_TIMEOUT == 1)
			/* Check Systick counter flag to decrement the time-out value */
			if (LL_SYSTICK_IsActiveCounterFlag())
			{
				if (Timeout-- == 0)
				{
					/* Time-out occurred. Set LED to blinking mode */
					ERROR("ADC Timeout");
				}
			}
			#endif /* USE_TIMEOUT */
		}

		/* Delay between ADC end of calibration and ADC enable.                   */
		/* Note: Variable divided by 2 to compensate partially                    */
		/*       CPU processing cycles (depends on compilation optimization).     */
		/*wait_loop_index = (ADC_DELAY_CALIB_ENABLE_CPU_CYCLES >> 1);
		while(wait_loop_index != 0)
		{
		  wait_loop_index--;
		}*/
		HAL_Delay(1);

		/* Restore ADC DMA transfer request after calibration */
		//LL_ADC_REG_SetDMATransfer(_hRes->instance, backup_setting_adc_dma_transfer);

		ConfigureDMA();

		/* Enable ADC */
		LL_ADC_Enable(_hRes->instance);

		/* Poll for ADC ready to convert */
		#if (USE_TIMEOUT == 1)
		Timeout = ADC_ENABLE_TIMEOUT_MS;
		#endif /* USE_TIMEOUT */

		while (LL_ADC_IsActiveFlag_ADRDY(_hRes->instance) == 0)
		{
			#if (USE_TIMEOUT == 1)
			/* Check Systick counter flag to decrement the time-out value */
			if (LL_SYSTICK_IsActiveCounterFlag())
			{
				if(Timeout-- == 0)
				{
					/* Time-out occurred. Set LED to blinking mode */
					ERROR("ADC Timeout");
				}
			}
			#endif /* USE_TIMEOUT */
		}
	}

	HAL_Delay(10);

	// Start ADC
	if ((LL_ADC_IsEnabled(_hRes->instance) == 1) && (LL_ADC_IsDisableOngoing(_hRes->instance) == 0) && (LL_ADC_REG_IsConversionOngoing(_hRes->instance) == 0)   )
	{
		LL_ADC_REG_StartConversion(_hRes->instance);
	}
	else
	{
		/* Error: ADC conversion start could not be performed */
		ERROR("Could not start ADC conversion")
	}

	/* Retrieve ADC conversion data */
	  /* (data scale corresponds to ADC resolution: 12 bits) */
	//_hRes->buffer[0] = LL_ADC_REG_ReadConversionData12(_hRes->instance);

	 /* Clear flag ADC group regular end of unitary conversion */
	    /* Note: This action is not needed here, because flag ADC group regular   */
	    /*       end of unitary conversion is cleared automatically when          */
	    /*       software reads conversion data from ADC data register.           */
	    /*       Nevertheless, this action is done anyway to show how to clear    */
	    /*       this flag, needed if conversion data is not always read          */
	    /*       or if group injected end of unitary conversion is used (for      */
	    /*       devices with group injected available).                          */
	//LL_ADC_ClearFlag_EOC(_hRes->instance);
}

void ADC::ConfigureADCGPIO()
{
	if (!_hRes) return;

	LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Mode = LL_GPIO_MODE_ANALOG;
	GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
	GPIO_InitStruct.Speed = LL_GPIO_SPEED_LOW;

	if (_hRes->adc == ADC_1)
	{
		/**ADC1 GPIO Configuration
		PA0     ------> ADC1_IN0
		PA1     ------> ADC1_IN1
		PA2     ------> ADC1_IN2
		PB1     ------> ADC1_IN9
		*/
		if (_channel == ADC_CHANNEL_0) {
			LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
			GPIO_InitStruct.Pin = LL_GPIO_PIN_0;
			LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
		}
		else if (_channel == ADC_CHANNEL_1) {
			LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
			GPIO_InitStruct.Pin = LL_GPIO_PIN_1;
			LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
		}
		else if (_channel == ADC_CHANNEL_2) {
			LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
			GPIO_InitStruct.Pin = LL_GPIO_PIN_2;
			LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
		}
		else {
			_hRes = 0;
			ERROR("Invalid ADC channel");
			return;
		}
	}
}

#if 0
void ADC::ConfigureADCChannels()
{
	if (!_hRes) return;

	ADC_ChannelConfTypeDef sConfig = {0};

	/*sConfig.Channel = _channel;
	sConfig.Rank = ADC_REGULAR_RANK_1; // Rank of sampled channel number ADCx_CHANNEL
	sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5; // Sampling time (number of clock cycles unit)
	sConfig.SingleDiff = ADC_SINGLE_ENDED;
	sConfig.OffsetNumber = ADC_OFFSET_NONE;
	sConfig.Offset = 0;
	if (HAL_ADC_ConfigChannel(&_hRes->handle, &sConfig) != HAL_OK)
	{
		_hRes = 0;
		ERROR("Could not configure ADC channel");
		return;
	}

	// Start continuous ADC conversion
	if (HAL_ADC_Start(&_hRes->handle) != HAL_OK)
	{
		_hRes = 0;
		ERROR("Could not start ADC");
		return;
	}*/

	/* Loop through configured channels and configure rank according to map */
	for (int channel = 0; channel < sizeof(_hRes->map_channel2bufferIndex); channel++) {
		if (_hRes->map_channel2bufferIndex[channel] != 0xFF) {
			sConfig.Channel      = channel;                /* Sampled channel number */
			sConfig.Rank         = _hRes->map_channel2bufferIndex[channel] + 1; // ADC_REGULAR_RANK_1;          /* Rank of sampled channel number ADCx_CHANNEL */
			sConfig.SamplingTime = ADC_SAMPLETIME_8CYCLES_5;   /* Sampling time (number of clock cycles unit) */
			if (channel == ADC_CHANNEL_VBAT_DIV4 || channel == ADC_CHANNEL_TEMPSENSOR || channel == ADC_CHANNEL_VREFINT)
				sConfig.SamplingTime = ADC_SAMPLETIME_387CYCLES_5;   /* Sampling time (number of clock cycles unit) */
			sConfig.SingleDiff   = ADC_SINGLE_ENDED;            /* Single-ended input channel */
			sConfig.OffsetNumber = ADC_OFFSET_NONE;             /* No offset subtraction */
			sConfig.Offset = 0;                                 /* Parameter discarded because offset correction is disabled */
			if (HAL_ADC_ConfigChannel(&_hRes->handle, &sConfig) != HAL_OK)
			{
				_hRes = 0;
				ERROR("Could not configure ADC channel");
				return;
			}
		}
	}

	  _hRes->bufferSize = 2*_hRes->numberOfConfiguredChannels; // 16-bit pr. channel = 2 bytes pr. channel

	  /* ### - 4 - Start conversion in DMA mode ################################# */
	  //if (StartDMA(&_hRes->handle,
	  if (HAL_ADC_Start_DMA(&_hRes->handle,
							(uint32_t *)_hRes->buffer,
							_hRes->numberOfConfiguredChannels  // just sample the number of channels into the buffer
						   ) != HAL_OK)
	  {
			_hRes = 0;
			ERROR("Could not start ADC");
			return;
	  }
}
#endif

void ADC::ConfigureDMA(void)
{
	// See DMA mapping on page 226 in Reference Manual
	// Note that the below DMA and Stream seems incorrect when compared to this table?!

	CLEAR_BIT(SYSCFG->CFGR1, ((uint32_t)1 << 8));

	// Enable DMA clock
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

	/* Configure the DMA transfer */
	/*  - DMA transfer in circular mode to match with ADC configuration:        */
	/*    DMA unlimited requests.                                               */
	/*  - DMA transfer from ADC without address increment.                      */
	/*  - DMA transfer to memory with address increment.                        */
	/*  - DMA transfer from ADC by half-word to match with ADC configuration:   */
	/*    ADC resolution 12 bits.                                               */
	/*  - DMA transfer to memory by half-word to match with ADC conversion data */
	/*    buffer variable type: half-word.                                      */
	LL_DMA_ConfigTransfer(DMA1,
	                      LL_DMA_CHANNEL_1,
	                      LL_DMA_DIRECTION_PERIPH_TO_MEMORY |
	                      LL_DMA_MODE_CIRCULAR              |
	                      LL_DMA_PERIPH_NOINCREMENT         |
	                      LL_DMA_MEMORY_INCREMENT           |
	                      LL_DMA_PDATAALIGN_HALFWORD        |
	                      LL_DMA_MDATAALIGN_HALFWORD        |
	                      LL_DMA_PRIORITY_HIGH               );

	/* Set DMA transfer addresses of source and destination */
	LL_DMA_ConfigAddresses(DMA1,
	                       LL_DMA_CHANNEL_1,
	                       LL_ADC_DMA_GetRegAddr(_hRes->instance, LL_ADC_DMA_REG_REGULAR_DATA),
						   (uint32_t)_hRes->buffer,
	                       LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

	/* Set DMA transfer size */
	LL_DMA_SetDataLength(DMA1,
	                     LL_DMA_CHANNEL_1,
						 _hRes->numberOfConfiguredChannels);

	/* Enable DMA transfer interruption: transfer complete */
	LL_DMA_EnableIT_TC(DMA1,
	                   LL_DMA_CHANNEL_1);

	/* Enable DMA transfer interruption: transfer error */
	LL_DMA_EnableIT_TE(DMA1,
	                   LL_DMA_CHANNEL_1);

	/*## Activation of DMA #####################################################*/
	/* Enable the DMA transfer */
	LL_DMA_EnableChannel(DMA1,
	                     LL_DMA_CHANNEL_1);

	/* NVIC configuration for DMA Input data interrupt */
	NVIC_SetPriority(DMA1_Channel1_IRQn, 1);  // DMA IRQ needs to be a lower priority than ADC IRQ
	NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

void ADC1_IRQHandler(void)
{
	if (!ADC::resADC1) return;

	/* Check whether ADC group regular end of sequence conversions caused       */
	/* the ADC interruption.                                                    */
	if(LL_ADC_IsActiveFlag_EOS(ADC1) != 0)
	{
		/* Clear flag ADC group regular end of sequence conversions */
		LL_ADC_ClearFlag_EOS(ADC1);

#if 0
		/* Update status variable of ADC group regular sequence */
		ubAdcGrpRegularSequenceConvStatus = 1;
		ubAdcGrpRegularSequenceConvCount++;
#endif
	}

	/* Check whether ADC group regular overrun caused the ADC interruption */
	if(LL_ADC_IsActiveFlag_OVR(ADC1) != 0)
	{
		/* Clear flag ADC group regular overrun */
		LL_ADC_ClearFlag_OVR(ADC1);

		/* Disable ADC group regular overrun interruption */
		LL_ADC_DisableIT_OVR(ADC1);

		/* Error from ADC */
		ERROR("ADC Overrun")
	}

	/* Check whether ADC group regular overrun caused the ADC interruption */
	if(LL_ADC_IsActiveFlag_EOC(ADC1) != 0)
	{
		/* Clear flag ADC group regular overrun */
		LL_ADC_ClearFlag_EOC(ADC1);
	}
}

void DMA1_Channel1_IRQHandler(void)
{
	if (!ADC::resADC1) return;

	/* Check whether DMA transfer complete caused the DMA interruption */
	if(LL_DMA_IsActiveFlag_TC1(DMA1) == 1)
	{
		/* Clear flag DMA global interrupt */
		/* (global interrupt flag: half transfer and transfer complete flags) */
		LL_DMA_ClearFlag_GI1(DMA1);

#if 0
		/* For this example purpose, check that DMA transfer status is matching     */
		/* ADC group regular sequence status:                                       */
		if (ubAdcGrpRegularSequenceConvStatus != 1)
		{
			AdcDmaTransferError_Callback();
		}

		/* Reset status variable of ADC group regular sequence */
		ubAdcGrpRegularSequenceConvStatus = 0;
#endif
	}

	/* Check whether DMA transfer error caused the DMA interruption */
	if(LL_DMA_IsActiveFlag_TE1(DMA1) == 1)
	{
		/* Clear flag DMA transfer error */
		LL_DMA_ClearFlag_TE1(DMA1);

		/* Error detected during DMA transfer */
		ERROR("DMA transfer error");
	}
}

int32_t ADC::ReadRaw()
{
	if (_hRes->map_channel2bufferIndex[_channel] == 0xFF) return -1; // error, channel not configured
	return _hRes->buffer[_hRes->map_channel2bufferIndex[_channel]];
}

// Return ADC reading between 0-1, where 1 corresponds to the ADC's Analog reference (Aref)
float ADC::Read()
{
	int32_t reading = ReadRaw();
	if (reading >= 0) {
		float converted = (float)reading / _hRes->range;
		return converted;
	} else {
		return 0;
	}
}

int32_t ADC::Read_mVolt()
{
	if (_hRes->map_channel2bufferIndex[_channel] == 0xFF) return -1; // error, channel not configured

	uint16_t VREF = __LL_ADC_CALC_VREFANALOG_VOLTAGE(_hRes->buffer[_hRes->map_channel2bufferIndex[ADC_CHANNEL_VREFINT]], _hRes->resolution);
	uint16_t mVolt = __LL_ADC_CALC_DATA_TO_VOLTAGE(VREF, _hRes->buffer[_hRes->map_channel2bufferIndex[_channel]], _hRes->resolution);

	return mVolt;
}

// Return ADC reading in volt based on VREFINT correction (if ADC3 is enabled), otherwise based on 3.3V VREF+ assumption
float ADC::ReadVoltage()
{
	int32_t mVolt = Read_mVolt();
	if (mVolt > 0)
		return (float)mVolt / 1000.f; // convert mV to float Voltage
	else
		return 0.f; // error
}

uint16_t ADC::GetCoreTemperature(void)
{
	if (!ADC::resADC1) return 0; // only available if ADC is configured

	uint16_t VREF = __LL_ADC_CALC_VREFANALOG_VOLTAGE(ADC::resADC1->buffer[ADC::resADC1->map_channel2bufferIndex[ADC_CHANNEL_VREFINT]], ADC::resADC1->resolution);
	uint16_t Temp_CelsiusDegree = __LL_ADC_CALC_TEMPERATURE(VREF, ADC::resADC1->buffer[ADC::resADC1->map_channel2bufferIndex[ADC_CHANNEL_TEMPSENSOR]], ADC::resADC1->resolution);

    return Temp_CelsiusDegree;
}

uint16_t ADC::GetVREFINTraw(void)
{
	if (!ADC::resADC1) return 0; // only available if ADC is configured

	return resADC1->buffer[resADC1->map_channel2bufferIndex[ADC_CHANNEL_VREFINT]];
}

float ADC::GetVREF(void)
{
	if (!ADC::resADC1) return 0; // only available if ADC is configured

	uint16_t VREF = __LL_ADC_CALC_VREFANALOG_VOLTAGE(resADC1->buffer[resADC1->map_channel2bufferIndex[ADC_CHANNEL_VREFINT]], resADC1->resolution);

    return (float)VREF / 1000.f; // convert mV to float Voltage
}